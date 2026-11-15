/**

  @file       mram_config.h
  @brief      Every editable MRAM value: the fitted part, register configuration, timings and
              bench test parameters.
  @details    Values are written with the register field names from the part headers rather than
              raw hex, so each one says what it sets. Each part has two register sets:
              - FACTORY_*: what the datasheet says a new chip ships with. Reference only, the
                driver never writes these.
              - FLIGHT_*: what the driver checks for (VerifyConfig()) and writes (ApplyConfig()).

              The status register is in the factory set but deliberately not in the flight set:
              block protection is a decision about the image layout, not a driver default.

  @author     Viola Case
  @date       15.11.2026
  @copyright  © Viola Case, 2026. All rights reserved.

**/
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

#include <OwlSat/mram_as3016204.h>
#include <OwlSat/mram_as3064204.h>
#include <OwlSat/mram_core.h>

namespace OwlSat::Config {

  /*!
   * The MRAM part the firmware drives. Flip to MRAM::AS3064204 for a board carrying the new part.
   * MRAM::Init() checks the chip's RDID against it.
   */
  using MRAMPart = MRAM::AS3016204;

  /*!
   * Size of Execute()'s transfer buffer: opcode, address, latency and data bytes for one CS# cycle.
   * Longer reads and writes are split into several instructions. It lives on the stack.
   */
  inline constexpr size_t MRAM_MAX_BELT_SIZE = 256;

  /*!
   * Whether the flight design executes in place (XIP) from the MRAM.
   *
   * - true: the image runs from the MRAM (the SDK's default binary type). Every driver
   *   transaction takes the chip out of XIP, runs its CS# cycles from SRAM with interrupts off
   *   (flash_safe_execute), then restores XIP the way the bootrom set it up.
   * - false: the image runs from SRAM (`pico_set_binary_type(<target> copy_to_ram)`) and the MRAM
   *   is only storage. The driver drives the QMI directly and never touches XIP.
   *
   * The build checks this against the binary type below. SRAM-only bench binaries (no_flash)
   * always drive the QMI directly, whatever this says.
   */
  inline constexpr bool MRAM_EXECUTE_IN_PLACE = true;

#if !PICO_NO_FLASH && !PICO_COPY_TO_RAM
  static_assert(MRAM_EXECUTE_IN_PLACE, "This binary executes from the MRAM, so the driver must leave and restore XIP around every "
                                       "transaction. Set MRAM_EXECUTE_IN_PLACE, or build the target as copy_to_ram.");
#endif

  /// Whether the driver wraps its transactions in an XIP exit / restore. Derived, don't edit
#if PICO_NO_FLASH
  inline constexpr bool MRAM_USES_XIP_SESSIONS = false;
#else
  inline constexpr bool MRAM_USES_XIP_SESSIONS = MRAM_EXECUTE_IN_PLACE;
#endif

  /*!
   * QMI direct-mode SCK = clk_sys / this, for every driver transaction. 30 gives 5 MHz at
   * 150 MHz: the divisor the SDK's own boot2 uses for direct-mode commands, and a tenth of the
   * slowest instruction limit the driver uses (READ 03h at 50 MHz). DIRECT_CSR.CLKDIV is 8 bits.
   */
  inline constexpr uint32_t MRAM_DIRECT_CLKDIV = 30;
  static_assert(MRAM_DIRECT_CLKDIV >= 1 && MRAM_DIRECT_CLKDIV <= 255, "QMI DIRECT_CSR.CLKDIV is 8 bits, 1..255");

  /*!
   * How long flash_safe_execute() may wait to get the other core and interrupts out of the way
   * before a transaction. Only used when MRAM_USES_XIP_SESSIONS.
   */
  inline constexpr uint32_t MRAM_SAFE_EXECUTE_TIMEOUT_MS = 10;

  /*!
   * Register configuration and timings for one part. Specialised for each supported part below.
   *
   * @tparam Part  MRAM::AS3016204 or MRAM::AS3064204
   */
  template <typename Part>
  struct MRAMSettings;

  /*!
   * AS3016204 settings. Table numbers are from its datasheet, revision P.
   */
  template <>
  struct MRAMSettings<MRAM::AS3016204> {
    using Part = MRAM::AS3016204; //!< The part these settings are for

    /*
     * ===========================================
     * Factory register values (Tables 12, 20, 21, 26, 27)
     * ===========================================
     */
    static constexpr uint8_t FACTORY_SR = 0x00;                                                  //!< No protection
    static constexpr uint8_t FACTORY_CR1 = 0x00;                                                 //!< No locks
    static constexpr uint8_t FACTORY_CR2 = Part::CR2::EncodeMLATS(0);                            //!< SPI, no read latency
    static constexpr uint8_t FACTORY_CR3 = Part::CR3::EncodeODSEL(3) | Part::CR3::WRPLS_16;      //!< 45Ω on a 3.0 V part, no wrap
    static constexpr uint8_t FACTORY_CR4 = Part::CR4::MUST_BE_ONE | Part::CR4::WRENS_SRAM;       //!< Array writes don't need WREN

    /// Factory values as a table, for comparing against the chip
    static constexpr std::array<MRAM::RegisterDefault, 5> FACTORY_DEFAULTS { {
      { Part::SR::ADDRESS, FACTORY_SR, Part::SR::WRITABLE },
      { Part::CR1::ADDRESS, FACTORY_CR1, Part::CR1::WRITABLE },
      { Part::CR2::ADDRESS, FACTORY_CR2, Part::CR2::WRITABLE },
      { Part::CR3::ADDRESS, FACTORY_CR3, Part::CR3::WRITABLE },
      { Part::CR4::ADDRESS, FACTORY_CR4, Part::CR4::WRITABLE },
    } };

    /*
     * ===========================================
     * Flight register values
     * ===========================================
     */

    /// Lock the block protection set in the status register
    static constexpr uint8_t FLIGHT_CR1 = Part::CR1::MAPLK;

    /*!
     * No read latency. The driver reads the array with READ 03h, which doesn't use it, and the
     * boot path's read settings depend on this register, so it stays as it was.
     */
    static constexpr uint8_t FLIGHT_CR2 = Part::CR2::EncodeMLATS(0);

    /// Factory output drive, no read wrap
    static constexpr uint8_t FLIGHT_CR3 = FACTORY_CR3;

    /// Every array write needs its own WREN, so a stray write instruction can't land
    static constexpr uint8_t FLIGHT_CR4 = Part::CR4::MUST_BE_ONE | Part::CR4::WRENS_NORMAL;

    /// Flight values as a table, for VerifyConfig() and ApplyConfig()
    static constexpr std::array<MRAM::RegisterDefault, 4> FLIGHT_DEFAULTS { {
      { Part::CR1::ADDRESS, FLIGHT_CR1, Part::CR1::WRITABLE },
      { Part::CR2::ADDRESS, FLIGHT_CR2, Part::CR2::WRITABLE },
      { Part::CR3::ADDRESS, FLIGHT_CR3, Part::CR3::WRITABLE },
      { Part::CR4::ADDRESS, FLIGHT_CR4, Part::CR4::WRITABLE },
    } };

    /*
     * ===========================================
     * Timings
     * ===========================================
     */
    static constexpr uint64_t POWER_UP_US = 250;              //!< tPU: power-up to first instruction
    static constexpr uint32_t REGISTER_WRITE_DESELECT_US = 5; //!< tCS2: CS# high after a register write
    static constexpr uint32_t ARRAY_WRITE_DESELECT_US = 1;    //!< tCS3: CS# high after an array write (280 ns, rounded up)
  };

  /*!
   * AS3064204 settings. Table numbers are from its datasheet, revision C.8.
   */
  template <>
  struct MRAMSettings<MRAM::AS3064204> {
    using Part = MRAM::AS3064204; //!< The part these settings are for

    /*
     * ===========================================
     * Factory register values (Tables 15, 21, 22)
     * ===========================================
     */
    static constexpr uint8_t FACTORY_SR = 0x00;                                                     //!< No protection
    static constexpr uint8_t FACTORY_CR1 = Part::CR1::EncodeODSEL(0b110) | Part::CR1::WRENS_NORMAL; //!< 30Ω at 3.0 V, no lock, WREN before every array write
    static constexpr uint8_t FACTORY_CR2 = Part::CR2::EncodeMLATS(8);                               //!< XIP writes enabled, 8 latency cycles

    /// Factory values as a table, for comparing against the chip
    static constexpr std::array<MRAM::RegisterDefault, 3> FACTORY_DEFAULTS { {
      { Part::SR::ADDRESS, FACTORY_SR, Part::SR::WRITABLE },
      { Part::CR1::ADDRESS, FACTORY_CR1, Part::CR1::WRITABLE },
      { Part::CR2::ADDRESS, FACTORY_CR2, Part::CR2::WRITABLE },
    } };

    /*
     * ===========================================
     * Flight register values
     * ===========================================
     */

    /*!
     * Factory values. MAPLK stays clear, so ApplyConfig() can't lock block protection until that's
     * an explicit decision.
     */
    static constexpr uint8_t FLIGHT_CR1 = FACTORY_CR1;

    /*!
     * 8 latency cycles: one dummy byte in 1-1-1, which is how the driver reads registers on this
     * part. Must stay a multiple of 8 (see the static_assert below).
     */
    static constexpr uint8_t FLIGHT_CR2 = FACTORY_CR2;

    /// Flight values as a table, for VerifyConfig() and ApplyConfig()
    static constexpr std::array<MRAM::RegisterDefault, 2> FLIGHT_DEFAULTS { {
      { Part::CR1::ADDRESS, FLIGHT_CR1, Part::CR1::WRITABLE },
      { Part::CR2::ADDRESS, FLIGHT_CR2, Part::CR2::WRITABLE },
    } };

    /*
     * ===========================================
     * Timings (Tables 10, 33)
     * ===========================================
     */
    static constexpr uint64_t POWER_UP_US = 2500;             //!< tPU: power-up or reset to first instruction
    static constexpr uint32_t REGISTER_WRITE_DESELECT_US = 5; //!< tCS2: CS# high after a register write
    static constexpr uint32_t ARRAY_WRITE_DESELECT_US = 1;    //!< tCS3: CS# high after an array write (280 ns, rounded up)
  };

  /// Settings for the part the firmware drives
  using MRAMActiveSettings = MRAMSettings<MRAMPart>;

  /*!
   * @brief   Checks one part's settings against what the driver can do.
   * @tparam  Part  The part to check
   * @return  Always true. The checks are static_asserts
   */
  template <typename Part>
  consteval bool CheckMRAMSettings() {
    using Settings = MRAMSettings<Part>;
    static_assert(Part::CAPACITY <= (size_t { 1 } << 24), "The driver sends 24-bit addresses");
    static_assert(Part::CR2::DecodeMLATS(Settings::FLIGHT_CR2) % 8 == 0,
                  "Execute() sends latency as whole dummy bytes in 1-1-1, so MLATS must be a multiple of 8");
    for (const MRAM::RegisterDefault &reg : Settings::FLIGHT_DEFAULTS) {
      if ((reg.value & Part::ForceOneBits(reg.address)) != Part::ForceOneBits(reg.address)) return false;
    }
    return true;
  }
  static_assert(CheckMRAMSettings<MRAM::AS3016204>(), "A flight register value is missing a must-be-one bit");
  static_assert(CheckMRAMSettings<MRAM::AS3064204>(), "A flight register value is missing a must-be-one bit");

  /*!
   * Parameters for the MRAM_Test_Read, MRAM_Test_Write and MRAM_Test_XIP bench binaries
   * (mram-test/). Not used by the flight image.
   */
  namespace MRAMTest {
    inline constexpr uint32_t STARTUP_DELAY_MS = 2000; //!< Pause before the first line, so a serial reader can attach
    inline constexpr uint32_t SUMMARY_PERIOD_MS = 5000; //!< How often the summary repeats once the tests are done

    inline constexpr uint32_t READ_ADDRESS = 0x000000; //!< Read test start: the bottom of the array, where blinky lives
    inline constexpr size_t READ_LENGTH = 300;         //!< Read test length. Longer than one transfer, so it crosses a chunk boundary

    /// Write test start: inside the unprotected upper 1 MiB of the test board's AS3016204
    inline constexpr uint32_t WRITE_ADDRESS = 0x1FF000;
    inline constexpr size_t WRITE_LENGTH = 300; //!< Write test length. Longer than one transfer, so it crosses a chunk boundary

    /// Stack for MRAM_Test_XIP's test task, in words. Holds three write-length buffers, a transfer belt and printf
    inline constexpr uint32_t TEST_TASK_STACK_WORDS = 2048;
  } // namespace MRAMTest

  /*!
   * Pre-flight storage mode: the state the MRAM_Preflight bench binary (mram-preflight/) leaves the
   * chip in before the board is put away for launch. Not used by the flight image.
   *
   * Provisional, and expected to change as the pre-launch state is decided. Today it is the flight
   * register configuration and nothing else.
   */
  namespace MRAMPreflight {
    /*!
     * Registers to write, in order, each verified against its mask. Starts as the flight set; replace
     * it with a table of its own when the two diverge.
     *
     * Order matters if SR is ever added: CR1.MAPLK locks SR.TBSEL/BPSEL, so SR has to come before
     * any entry that sets MAPLK, or its write will not verify.
     */
    inline constexpr auto &REGISTERS = MRAMActiveSettings::FLIGHT_DEFAULTS;

    /// Scratch area for the after-the-fact write check. Saved first and restored, so nothing changes
    inline constexpr uint32_t SCRATCH_ADDRESS = MRAMTest::WRITE_ADDRESS;
    inline constexpr size_t SCRATCH_LENGTH = MRAMTest::WRITE_LENGTH;
  } // namespace MRAMPreflight

} // namespace OwlSat::Config
