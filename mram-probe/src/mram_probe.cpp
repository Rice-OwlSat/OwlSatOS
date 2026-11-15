/**

    @file      mram_probe.cpp
    @brief     Bench probe: does the AS3016204's QPI/DPI interface mode survive a reset, or a power cycle?
    @details   SRAM-only (no_flash) REPL over USB serial, for the modified WeAct RP2350A test board with the
               MRAM on QMI chip select 0. Test procedure and results table: docs/mram_mode_probe.md.

               The chip's mode is unknown at boot, so nothing goes out on the QSPI bus until a command asks
               for it. The only opcodes this program ever sends are RDID (9Fh), RDSR (05h), RDCX (46h),
               QPIE (38h), DPIE (37h) and SPIE (FFh). There is deliberately no write, reset or power-down
               command. The notes for the older register-poking probe are in ../register_access_notes.txt.

               Never call rom_flash_exit_xip() from here. On RP2350 it sends the bootrom's own QPI/XIP exit
               sequence to CS0 (see its doc comment in pico/bootrom.h), which would put the chip back in SPI
               behind the experiment's back. rom_connect_internal_flash() only sets up the pads, so it's fine.

    @author    Viola Case
    @date      28.10.2026
    @copyright © Viola Case, 2026. All right reserved.

**/

#include <array>
#include <bit>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <span>

#include <hardware/address_mapped.h>
#include <hardware/clocks.h>
#include <hardware/structs/pads_qspi.h>
#include <hardware/structs/qmi.h>
#include <hardware/structs/timer.h>
#include <hardware/watchdog.h>
#include <pico/bootrom.h>
#include <pico/stdlib.h>

#include <OwlSat/mram.h>


namespace {

  using OwlSat::MRAM::Execute;
  using OwlSat::MRAM::Instruction;
  using OwlSat::MRAM::Opcode;
  namespace CR2 = OwlSat::MRAM::CR2;

  // Test-board LEDs. Not taken from OwlSat::Pins: on the flight harness GPIO 24 is BURN_0.
  constexpr uint LED_MATCH = 25;    //!< Solid: the last RDID read E6 01 04 01
  constexpr uint LED_MISMATCH = 24; //!< Solid: the last RDID read anything else

  /// RDID reply in SPI mode
  constexpr std::array<uint8_t, 4> EXPECTED_ID { 0xE6, 0x01, 0x04, 0x01 };

  /*!
   * Direct-mode SCK = clk_sys / DIRECT_CLKDIV, so 5 MHz at the default 150 MHz. That's the conservative
   * divisor the SDK's boot2_w25q080.S picks for its own direct-mode commands, and about a tenth of the
   * 54 MHz register-read limit. DIRECT_CSR.CLKDIV is 8 bits, 1..255 encoded directly.
   */
  constexpr uint32_t DIRECT_CLKDIV = 30;
  static_assert(DIRECT_CLKDIV >= 1 && DIRECT_CLKDIV <= 255);

  constexpr uint32_t ESCAPE_SETTLE_US = 10; //!< Pause after each of the escape's CS# cycles

  /*!
   * QPIE and DPIE are deliberately not in the driver's Instruction set, so the probe makes its own by
   * bit-casting this mirror of Instruction's private members. The asserts check the mirror against
   * instructions the driver does define, so a change to Instruction's layout fails the build here.
   */
  struct InstructionBits {
    Opcode opcode;
    size_t address_width;
    size_t latency_cycles;
    bool needs_write_enable;

    constexpr bool operator==(const InstructionBits &) const = default;
  };
  static_assert(sizeof(InstructionBits) == sizeof(Instruction) && alignof(InstructionBits) == alignof(Instruction));
  static_assert(std::bit_cast<InstructionBits>(Instruction::ReadDeviceID) == InstructionBits { Opcode::RDID, 0, 0, false });
  static_assert(std::bit_cast<InstructionBits>(Instruction::ReadAnyRegister) == InstructionBits { Opcode::RDAR, 3, 8, false });
  static_assert(std::bit_cast<InstructionBits>(Instruction::WriteAnyRegister) == InstructionBits { Opcode::WRAR, 3, 0, true });

  constexpr Instruction ENABLE_QPI = std::bit_cast<Instruction>(InstructionBits { Opcode::QPIE, 0, 0, false }); //!< 1-0-0. 4-4-4 from the next CS# cycle
  constexpr Instruction ENABLE_DPI = std::bit_cast<Instruction>(InstructionBits { Opcode::DPIE, 0, 0, false }); //!< 1-0-0. 2-2-2 from the next CS# cycle

  // pads_qspi_hw->io[] runs SCLK, SD0, SD1, SD2, SD3, SS
  constexpr size_t PAD_SD2 = 3;
  constexpr size_t PAD_SD3 = 4;
  static_assert(PADS_QSPI_GPIO_QSPI_SD2_OFFSET == PADS_QSPI_GPIO_QSPI_SCLK_OFFSET + 4 * PAD_SD2);
  static_assert(PADS_QSPI_GPIO_QSPI_SD3_OFFSET == PADS_QSPI_GPIO_QSPI_SCLK_OFFSET + 4 * PAD_SD3);


  /*
   * ===========================================
   * Bus access
   * ===========================================
   * MRAM::BUS_MODE is never updated, even after QPIE/DPIE. Execute() only drives the bus while it
   * reads M1_1_1, and this probe always wants to send single-wire commands, whatever mode the chip is
   * in. Getting garbage back is the measurement.
   */

  bool g_bus_ready = false;

  /*!
   * On the first command that needs the bus: connect the QMI to the QSPI pads and set the direct-mode
   * clock. Neither step clocks anything out. Leaves direct mode off and CS0# high.
   */
  void __not_in_flash_func(PrepareBus)() {
    if (g_bus_ready) return;
    rom_connect_internal_flash();
    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_CS0N_BITS);
    hw_write_masked(&qmi_hw->direct_csr, DIRECT_CLKDIV << QMI_DIRECT_CSR_CLKDIV_LSB, QMI_DIRECT_CSR_CLKDIV_BITS);
    g_bus_ready = true;
  }

  /// One single-wire instruction through the driver. False if the driver refused it.
  bool __not_in_flash_func(Run)(const Instruction &instr, std::span<uint8_t> rx) {
    PrepareBus();
    return Execute(instr, 0, {}, rx);
  }

  /// Busy-waits on the raw microsecond timer. Register reads only, so it's safe with XIP unavailable.
  void __not_in_flash_func(SettleUs)(uint32_t us) {
    uint32_t start = timer_hw->timerawl;
    while (timer_hw->timerawl - start <= us) {}
  }

  /*!
   * One byte in its own CS0# cycle, at a given interface width, with the RX push inhibited so nothing
   * lands in DIRECT_RX. Returns with CS0# high and direct mode off.
   *
   * @param iwidth  QMI_DIRECT_TX_IWIDTH_VALUE_S, _D or _Q
   */
  void __not_in_flash_func(SendRawByte)(uint8_t byte, uint32_t iwidth) {
    // OE is ignored at single width, where SD0 is always an output. At dual/quad width it drives SD0-1/SD0-3.
    uint32_t oe = iwidth == QMI_DIRECT_TX_IWIDTH_VALUE_S ? 0 : QMI_DIRECT_TX_OE_BITS;

    hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}                             // a memory-mapped transfer may still be finishing
    while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS)) (void)qmi_hw->direct_rx; // a full RX FIFO stalls the shifter
    hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS0N_BITS);

    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | oe | (iwidth << QMI_DIRECT_TX_IWIDTH_LSB) | byte;
    while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS)) {}
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}

    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS0N_BITS);
    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
  }

  /*!
   * Mode-independent return to SPI: SPIE (FFh) once at each width, each in its own CS# cycle.
   *
   *     quad    2 clocks, SD0-3 driven high   SPIE to a chip in QPI (4-4-4)
   *     dual    4 clocks, SD0-1 driven high   SPIE to a chip in DPI (2-2-2)
   *     single  8 clocks on SD0               SPIE to a chip in SPI, which changes nothing
   *
   * A chip in any other mode sees only part of a command byte before CS# rises, so each step is
   * harmless to the modes it isn't aimed at. Written out step by step, without a table, so nothing is
   * read from .rodata while the chip may be unreadable.
   *
   * Lifting this into an XIP build: run it from SRAM with interrupts off and nothing else using the XIP
   * window (in direct mode, memory-mapped reads return bus errors), then set XIP up again afterwards.
   */
  void __not_in_flash_func(EscapeToSPI)() {
    constexpr uint8_t SPIE = static_cast<uint8_t>(Opcode::SPIE);
    PrepareBus();
    SendRawByte(SPIE, QMI_DIRECT_TX_IWIDTH_VALUE_Q);
    SettleUs(ESCAPE_SETTLE_US);
    SendRawByte(SPIE, QMI_DIRECT_TX_IWIDTH_VALUE_D);
    SettleUs(ESCAPE_SETTLE_US);
    SendRawByte(SPIE, QMI_DIRECT_TX_IWIDTH_VALUE_S);
    SettleUs(ESCAPE_SETTLE_US);
  }


  /*
   * ===========================================
   * LEDs
   * ===========================================
   * Idle: both blink together, about 2 Hz. Blinky alternates them at 0.5 Hz, so the two can't be confused.
   * After an RDID: GPIO 25 solid for a match, GPIO 24 solid for anything else.
   */

  enum struct Leds { Idle, Match, Mismatch };
  Leds g_leds = Leds::Idle;

  void SetLeds(Leds state) {
    g_leds = state;
    gpio_put(LED_MATCH, state == Leds::Match);
    gpio_put(LED_MISMATCH, state == Leds::Mismatch);
  }

  void TickIdleLeds() {
    if (g_leds != Leds::Idle) return;
    bool on = !gpio_get_out_level(LED_MATCH);
    gpio_put(LED_MATCH, on);
    gpio_put(LED_MISMATCH, on);
  }


  /*
   * ===========================================
   * Commands
   * ===========================================
   */

  unsigned OpByte(Opcode op) { return static_cast<unsigned>(op); }

  void PrintBytes(std::span<const uint8_t> bytes) {
    for (uint8_t b : bytes) printf(" %02X", b);
  }

  /// RDID, printed and shown on the LEDs. True if it read E6 01 04 01.
  bool CheckID() {
    std::array<uint8_t, 4> id {};
    if (!Run(Instruction::ReadDeviceID, id)) {
      printf("RDID  %02Xh: refused by the driver (MRAM::BUS_MODE is not 1-1-1)\n", OpByte(Opcode::RDID));
      return false;
    }
    bool match = id == EXPECTED_ID;
    printf("RDID  %02Xh:", OpByte(Opcode::RDID));
    PrintBytes(id);
    printf(match ? "   match, the chip answers in SPI\n" : "   MISMATCH, expected E6 01 04 01\n");
    SetLeds(match ? Leds::Match : Leds::Mismatch);
    return match;
  }

  void PrintPad(const char *name, uint32_t pad) {
    printf("  %-14s %03X  pull-up %s, pull-down %s%s\n", name, static_cast<unsigned>(pad), pad & PADS_QSPI_GPIO_QSPI_SD2_PUE_BITS ? "on" : "off",
           pad & PADS_QSPI_GPIO_QSPI_SD2_PDE_BITS ? "on" : "off", pad & PADS_QSPI_GPIO_QSPI_SD2_ISO_BITS ? ", ISOLATED" : "");
  }

  void CmdRdid() { CheckID(); }

  void CmdRegs() {
    bool valid = CheckID();
    std::array<uint8_t, 1> sr {};
    std::array<uint8_t, 4> cr {};
    Run(Instruction::ReadStatusRegister, sr);
    Run(Instruction::ReadConfigurationRegisters, cr);

    printf("RDSR  %02Xh:", OpByte(Opcode::RDSR));
    PrintBytes(sr);
    printf("            SR\n");
    printf("RDCX  %02Xh:", OpByte(Opcode::RDCX));
    PrintBytes(cr);
    printf("   CR1 CR2 CR3 CR4\n");
    if (valid) {
      uint8_t cr2 = cr[1];
      printf("CR2: QPISL=%d DPISL=%d MLATS=%u\n", (cr2 & CR2::QPISL) != 0, (cr2 & CR2::DPISL) != 0,
             static_cast<unsigned>((cr2 & CR2::MLATS_MASK) >> CR2::MLATS_SHIFT));
    } else {
      printf("** RDID did not match, so these register bytes are MEANINGLESS **\n");
    }

    // IO2/WP# and IO3 have no pull-up inside the MRAM. At single width the QMI doesn't drive them,
    // so the RP2350 pad pulls are all that hold them.
    printf("QSPI pads:\n");
    PrintPad("SD2 (IO2/WP#)", pads_qspi_hw->io[PAD_SD2]);
    PrintPad("SD3 (IO3)", pads_qspi_hw->io[PAD_SD3]);
  }

  void EnterMode(const Instruction &instr, Opcode op, const char *mode) {
    Run(instr, {});
    printf("%02Xh sent, single-wire. The chip should now be in %s, so garbage below is EXPECTED:\n", OpByte(op), mode);
    if (CheckID()) printf("RDID still matches: the chip did NOT switch.\n");
  }

  void CmdQpie() { EnterMode(ENABLE_QPI, Opcode::QPIE, "QPI (4-4-4)"); }
  void CmdDpie() { EnterMode(ENABLE_DPI, Opcode::DPIE, "DPI (2-2-2)"); }

  void CmdEscape() {
    EscapeToSPI();
    printf("Sent %02Xh three times, %u us apart: quad (SD0-3 driven), dual (SD0-1 driven), single.\n", OpByte(Opcode::SPIE),
           static_cast<unsigned>(ESCAPE_SETTLE_US));
    CheckID();
  }

  void CmdWdreset() {
    printf("Watchdog reboot, power stays on. If the LEDs then alternate about once a second, blinky booted from the MRAM.\n");
    SetLeds(Leds::Idle);
    sleep_ms(100); // let the USB stack send that line
    watchdog_reboot(0, 0, 0);
    for (;;) tight_loop_contents();
  }

  void CmdBootsel() {
    printf("Rebooting into BOOTSEL, power stays on.\n");
    SetLeds(Leds::Idle);
    sleep_ms(100);
    reset_usb_boot(0, 0);
  }

  void CmdHelp();

  struct Command {
    const char *name;
    void (*run)();
    const char *help;
  };

  constexpr Command COMMANDS[] = {
    { "rdid", CmdRdid, "RDID (9Fh) single-wire. E6 01 04 01 means the chip is in SPI" },
    { "regs", CmdRegs, "RDID, then RDSR (05h) and RDCX (46h). Meaningless unless RDID matches" },
    { "qpie", CmdQpie, "QPIE (38h) single-wire, then RDID. Garbage expected" },
    { "dpie", CmdDpie, "DPIE (37h) single-wire, then RDID. Garbage expected" },
    { "escape", CmdEscape, "FFh at quad, dual, then single width, each in its own CS# cycle, then RDID" },
    { "wdreset", CmdWdreset, "watchdog reboot (reset without power loss, normal boot path)" },
    { "bootsel", CmdBootsel, "reboot straight into BOOTSEL (reset without power loss)" },
    { "help", CmdHelp, "this list" },
  };

  void CmdHelp() {
    printf("commands:\n");
    for (const Command &c : COMMANDS) printf("  %-8s %s\n", c.name, c.help);
  }

  /// Reads a line with echo and backspace, blinking the idle LEDs while it waits
  void ReadLine(char *buf, size_t cap) {
    size_t n = 0;
    for (;;) {
      int c = getchar_timeout_us(250'000);
      if (c == PICO_ERROR_TIMEOUT) {
        TickIdleLeds();
      } else if (c == '\r' || c == '\n') {
        printf("\n");
        buf[n] = '\0';
        return;
      } else if (c == 0x08 || c == 0x7F) {
        if (n) {
          --n;
          printf("\b \b");
        }
      } else if (isprint(c) && n + 1 < cap) {
        buf[n++] = static_cast<char>(c);
        putchar(c);
      }
    }
  }

  void Dispatch(char *line) {
    char *cmd = strtok(line, " \t");
    if (!cmd) return;
    for (char *p = cmd; *p; ++p) *p = static_cast<char>(tolower(*p));
    for (const Command &c : COMMANDS) {
      if (!strcmp(cmd, c.name)) {
        c.run();
        return;
      }
    }
    printf("unknown command '%s' (try help)\n", cmd);
  }

} // namespace


int main() {
  for (uint pin : { LED_MATCH, LED_MISMATCH }) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
  }
  stdio_init_all();

  // Nothing touches the QSPI bus until a command asks for it
  while (!stdio_usb_connected()) {
    TickIdleLeds();
    sleep_ms(250);
  }
  sleep_ms(500);

  uint32_t sys_hz = clock_get_hz(clk_sys);
  printf("\n=== AS3016204 interface-mode probe ===\n");
  printf("Nothing has been sent to the MRAM. Opcodes this probe can send: 9F 05 46 38 37 FF.\n");
  printf("Direct-mode SCK = clk_sys %u kHz / CLKDIV %u = %u kHz (CLKDIV is set on the first command)\n", static_cast<unsigned>(sys_hz / 1000),
         static_cast<unsigned>(DIRECT_CLKDIV), static_cast<unsigned>(sys_hz / DIRECT_CLKDIV / 1000));
  CmdHelp();

  char line[80];
  for (;;) {
    printf("> ");
    ReadLine(line, sizeof line);
    Dispatch(line);
  }
}
