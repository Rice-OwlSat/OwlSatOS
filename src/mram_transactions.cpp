/**

    @file      mram_transactions.cpp
    @brief     Raw SPI transactions with the MRAM.
    @details   Execute() and the byte transport under it. Linked into the flight image and into the
               bench binaries (MRAM_Probe, MRAM_Test_Read, MRAM_Test_Write, MRAM_Test_XIP).

               One Execute() call is one *session*: one or two CS# cycles (WREN, then the
               instruction) run back to back. How a session reaches the bus depends on the build:

               - Config::MRAM_USES_XIP_SESSIONS (the image executes from the MRAM): the session runs
                 from SRAM inside flash_safe_execute(), so no interrupt handler or other core can
                 fetch code from the MRAM meanwhile. It takes the chip out of XIP once, runs every
                 CS# cycle, flushes the XIP cache and restores XIP with the setup routine the
                 bootrom left in boot RAM. This mirrors the SDK's flash_do_cmd(), except that WREN
                 and the instruction share one XIP exit, so nothing can reach the chip between them.
               - Otherwise (no_flash bench binaries, copy_to_ram images): nothing executes from the
                 MRAM, so the QMI is driven directly. The bus is prepared on first use.

               Flash builds (XIP or copy_to_ram) must link hardware_flash and pico_flash. XIP builds
               must also tell pico_flash how the other core is kept off the MRAM, e.g.
               PICO_FLASH_ASSUME_CORE1_SAFE=1 when core 1 never runs.
    @author    Viola Case
    @date      24.09.2026
    @copyright © Viola Case, 2026. All right reserved.

**/

#include <algorithm>
#include <array>

#include <hardware/address_mapped.h>
#include <hardware/structs/qmi.h>
#include <pico/bootrom.h>

#include <OwlSat/mram.h>

#if !PICO_NO_FLASH
#include <hardware/regs/addressmap.h>
#include <hardware/structs/pads_qspi.h>
#include <hardware/xip_cache.h>
#include <pico/flash.h>
#endif


namespace OwlSat::MRAM {

  Mode BUS_MODE = Mode::M1_1_1;

  namespace {

    /// The driver's copy of CR2.MLATS. Starts at the flight value. Init() corrects it from the chip
    uint8_t g_mlats = Config::MRAMPart::CR2::DecodeMLATS(Config::MRAMActiveSettings::FLIGHT_CR2);

    /*!
     * One CS# low→high cycle of a session, full duplex and in place: buf[i] is sent, and the byte
     * clocked in at the same time replaces it.
     */
    struct Cycle {
      uint8_t *buf; //!< Bytes to send, overwritten with the bytes received. Must be in SRAM
      size_t count; //!< Bytes in the cycle
    };

    /*!
     * @brief   Runs one CS# cycle through the QMI's direct mode. The QMI must already be connected to
     *          the pads, with nothing reading the XIP window.
     * @details Lives in SRAM, because XIP sessions call it while the MRAM can't serve code. The QMI
     *          stalls the clock when DIRECT_RX is full, so TX and RX can be pipelined freely.
     * @param   cycle  The cycle to run
     */
    void __no_inline_not_in_flash_func(Transfer)(const Cycle &cycle) {
      const uint8_t *tx = cycle.buf;
      uint8_t *rx = cycle.buf;
      size_t tx_left = cycle.count, rx_left = cycle.count;
      hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS0N_BITS); // CS low
      hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);          // direct mode on
      while (tx_left || rx_left) {
        uint32_t csr = qmi_hw->direct_csr;
        if (tx_left && !(csr & QMI_DIRECT_CSR_TXFULL_BITS)) {
          qmi_hw->direct_tx = *tx++;
          --tx_left;
        }
        if (rx_left && !(csr & QMI_DIRECT_CSR_RXEMPTY_BITS)) {
          *rx++ = static_cast<uint8_t>(qmi_hw->direct_rx);
          --rx_left;
        }
      }
      hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);          // direct mode off
      hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS0N_BITS); // CS high
    }

    /// Sets the direct-mode clock divisor. Clocks nothing out
    void __no_inline_not_in_flash_func(SetDirectClock)() {
      hw_write_masked(&qmi_hw->direct_csr, Config::MRAM_DIRECT_CLKDIV << QMI_DIRECT_CSR_CLKDIV_LSB, QMI_DIRECT_CSR_CLKDIV_BITS);
    }

    /*
     * ===========================================
     * Direct sessions (nothing executes from the MRAM)
     * ===========================================
     */

    bool g_direct_bus_ready = false; //!< Whether PrepareDirectBus() has run

    /*!
     * @brief   First-use bus setup for direct sessions: connects the QMI to the QSPI pads, sets the
     *          direct-mode clock, and in flash builds (copy_to_ram) takes the chip out of whatever
     *          XIP read mode the bootrom left it in.
     * @details no_flash bench binaries skip the XIP exit: the bootrom's exit sequence would change
     *          the chip's interface mode behind the mode probe's back.
     */
    void PrepareDirectBus() {
      if (g_direct_bus_ready) return;
      rom_connect_internal_flash();
#if !PICO_NO_FLASH
      rom_flash_exit_xip();
#endif
      hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_CS0N_BITS);
      SetDirectClock();
      g_direct_bus_ready = true;
    }

    /*!
     * @brief   Runs a session's cycles straight on the QMI.
     * @param   cycles  The cycles, in order
     * @return  Always true
     */
    bool RunDirectSession(std::span<const Cycle> cycles) {
      PrepareDirectBus();
      for (const Cycle &cycle : cycles) Transfer(cycle);
      return true;
    }

#if !PICO_NO_FLASH
    /*
     * ===========================================
     * XIP sessions (the image executes from the MRAM)
     * ===========================================
     * Everything from the XIP exit to the XIP restore must run from SRAM or ROM and touch only SRAM:
     * no code, constants or literal pools in the MRAM. Hence the __not_in_flash_func attributes,
     * the ROM function pointers looked up before the exit, and Cycle buffers on the caller's stack.
     */

    /// Size of the bootrom's saved XIP setup routine in boot RAM (same as the SDK's BOOT2_SIZE_WORDS)
    constexpr size_t XIP_SETUP_WORDS = 64;

    /*!
     * SRAM copy of the XIP setup routine the bootrom saved in boot RAM. Running it restores the
     * read mode and clock the bootrom picked at boot (bootrom.h, rom_flash_range_erase notes).
     */
    uint32_t g_xip_setup[XIP_SETUP_WORDS];
    bool g_xip_setup_copied = false; //!< Whether g_xip_setup holds the routine

    /// What a session needs, gathered before XIP goes away
    struct XipSession {
      std::span<const Cycle> cycles;                    //!< The CS# cycles, in order
      rom_connect_internal_flash_fn connect_internal_flash; //!< ROM: mux the QSPI pads to the QMI
      rom_flash_exit_xip_fn flash_exit_xip;                 //!< ROM: chip back to serial command state
      rom_flash_flush_cache_fn flash_flush_cache;           //!< ROM: flush XIP cache, release CS# force
    };

    /*!
     * @brief   The session itself. Called by flash_safe_execute() with interrupts off.
     * @details Same sequence as the SDK's flash_do_cmd(): save the pad and QMI window 1 state, leave
     *          XIP, run the cycles, flush the cache, run the saved XIP setup, restore the state. The
     *          window 1 restore assumes nothing is configured on CS1 via FLASH_DEVINFO, which is true
     *          for OwlSat.
     * @param   param  The XipSession
     */
    void __no_inline_not_in_flash_func(RunXipSessionLocked)(void *param) {
      const XipSession &session = *static_cast<const XipSession *>(param);

      // Commit pending XIP writes before the flush, then save what the ROM calls clobber
      xip_cache_clean_all();
      std::array<uint32_t, count_of(pads_qspi_hw->io)> pads;
      for (size_t i = 0; i < pads.size(); ++i) pads[i] = pads_qspi_hw->io[i];
      const uint32_t cs1_timing = qmi_hw->m[1].timing, cs1_rcmd = qmi_hw->m[1].rcmd, cs1_rfmt = qmi_hw->m[1].rfmt;

      __compiler_memory_barrier();
      session.connect_internal_flash();
      session.flash_exit_xip();
      SetDirectClock();

      for (const Cycle &cycle : session.cycles) Transfer(cycle);

      session.flash_flush_cache();
      reinterpret_cast<void (*)()>(reinterpret_cast<uintptr_t>(g_xip_setup) + 1)(); // +1: Thumb
      __compiler_memory_barrier();

      for (size_t i = 0; i < pads.size(); ++i) pads_qspi_hw->io[i] = pads[i];
      qmi_hw->m[1].timing = cs1_timing;
      qmi_hw->m[1].rcmd = cs1_rcmd;
      qmi_hw->m[1].rfmt = cs1_rfmt;
    }

    /*!
     * @brief   Runs a session's cycles with XIP suspended around them.
     * @param   cycles  The cycles, in order. Their buffers must be in SRAM
     * @return  False if flash_safe_execute() couldn't make the MRAM safe to take away
     */
    bool RunXipSession(std::span<const Cycle> cycles) {
      if (!g_xip_setup_copied) {
        const volatile uint32_t *saved = reinterpret_cast<const volatile uint32_t *>(BOOTRAM_BASE);
        for (size_t i = 0; i < XIP_SETUP_WORDS; ++i) g_xip_setup[i] = saved[i];
        __compiler_memory_barrier();
        g_xip_setup_copied = true;
      }

      XipSession session {
        cycles,
        reinterpret_cast<rom_connect_internal_flash_fn>(rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH)),
        reinterpret_cast<rom_flash_exit_xip_fn>(rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP)),
        reinterpret_cast<rom_flash_flush_cache_fn>(rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE)),
      };
      if (!session.connect_internal_flash || !session.flash_exit_xip || !session.flash_flush_cache) return false;

      return flash_safe_execute(RunXipSessionLocked, &session, Config::MRAM_SAFE_EXECUTE_TIMEOUT_MS) == PICO_OK;
    }
#endif

    /*!
     * @brief   Runs a session on whichever transport this build uses.
     * @param   cycles  The cycles, in order. Their buffers must be in SRAM
     * @return  False if the session couldn't run
     */
    bool RunSession(std::span<const Cycle> cycles) {
#if !PICO_NO_FLASH
      if constexpr (Config::MRAM_USES_XIP_SESSIONS) return RunXipSession(cycles);
#endif
      return RunDirectSession(cycles);
    }

  } // namespace

  namespace detail {
    uint8_t MemoryLatency() { return g_mlats; }
    void SetMemoryLatency(uint8_t cycles) { g_mlats = static_cast<uint8_t>(cycles & 0x0Fu); }
  } // namespace detail

  size_t Instruction::HeaderBytes() const {
    size_t cycles = 0;
    if (m_latency == Latency::Fixed) cycles = m_latency_cycles;
    if (m_latency == Latency::MLATS) cycles = g_mlats;
    // 1-1-1 clocks one bit per cycle, and Transfer() works in whole bytes
    if (cycles % 8 != 0) return 0;
    return sizeof(Opcode) + m_address_width + cycles / 8;
  }

  size_t Instruction::MaxDataBytes() const {
    size_t header = HeaderBytes();
    return header == 0 ? 0 : Config::MRAM_MAX_BELT_SIZE - header;
  }

  bool Execute(const Instruction &instr, uintptr_t addr, std::span<const uint8_t> tx_data, std::span<uint8_t> rx_data) {
    // Only single-lane SPI is driven from here
    if (BUS_MODE != Mode::M1_1_1) return false;

    // Opcode, address MSB first, then 8 latency cycles per dummy byte (1-1-1)
    size_t headerLen = instr.HeaderBytes();
    size_t dataLen = std::max(tx_data.size(), rx_data.size());
    if (headerLen == 0 || instr.m_address_width > sizeof(addr) || dataLen > Config::MRAM_MAX_BELT_SIZE - headerLen) return false;

    // Built on the stack (SRAM) before the session starts. The reply overwrites the belt as it goes out
    std::array<uint8_t, Config::MRAM_MAX_BELT_SIZE> belt {};
    belt[0] = static_cast<uint8_t>(instr.m_opcode);
    for (size_t i = 0; i < instr.m_address_width; ++i)
      belt[sizeof(Opcode) + i] = static_cast<uint8_t>(addr >> (8 * (instr.m_address_width - 1 - i)));
    std::copy(tx_data.begin(), tx_data.end(), belt.begin() + headerLen);

    // WREN is only latched when CS# rises after it, so it needs its own CS# cycle
    uint8_t wren = static_cast<uint8_t>(Opcode::WREN);
    std::array<Cycle, 2> cycles { { { &wren, 1 }, { belt.data(), headerLen + dataLen } } };
    std::span<const Cycle> session(cycles);
    if (!instr.m_needs_write_enable) session = session.subspan(1);

    if (!RunSession(session)) return false;
    std::copy_n(belt.begin() + headerLen, rx_data.size(), rx_data.begin());
    return true;
  }

} // namespace OwlSat::MRAM
