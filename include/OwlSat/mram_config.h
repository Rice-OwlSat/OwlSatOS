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
   * Parameters for the MRAM_Test_Read and MRAM_Test_Write bench binaries (mram-test/). Not used by
   * the flight image.
   */
  namespace MRAMTest {
    /// Direct-mode SCK = clk_sys / this, so 5 MHz at 150 MHz. QMI DIRECT_CSR.CLKDIV is 8 bits, 1..255
    inline constexpr uint32_t DIRECT_CLKDIV = 30;

    inline constexpr uint32_t STARTUP_DELAY_MS = 2000; //!< Pause before the first line, so a serial reader can attach
    inline constexpr uint32_t SUMMARY_PERIOD_MS = 5000; //!< How often the summary repeats once the tests are done

    inline constexpr uint32_t READ_ADDRESS = 0x000000; //!< Read test start: the bottom of the array, where blinky lives
    inline constexpr size_t READ_LENGTH = 300;         //!< Read test length. Longer than one transfer, so it crosses a chunk boundary

    /// Write test start: inside the unprotected upper 1 MiB of the test board's AS3016204
    inline constexpr uint32_t WRITE_ADDRESS = 0x1FF000;
    inline constexpr size_t WRITE_LENGTH = 300; //!< Write test length. Longer than one transfer, so it crosses a chunk boundary
  } // namespace MRAMTest

} // namespace OwlSat::Config
