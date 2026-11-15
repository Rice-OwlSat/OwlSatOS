/**

  @file       mram.h
  @brief      SPI MRAM chip driver: the nonvolatile store.
  @details    Driver API for the Avalanche serial MRAM on QMI chip select 0. Supports the AS3016204
              (16 Mb / 2 MiB, fitted to the test board, works over SPI out of the box, bench-tested
              23.09.2026) and the AS3064204 (64 Mb / 8 MiB, the prospective part). Which one is
              driven is chosen at compile time by Config::MRAMPart in mram_config.h.

              The API only uses what both parts have in common: register access by address (RDAR
              65h / WRAR 71h), array access with READ 03h / WRTE 02h, and RDID 9Fh. Part-specific
              instructions and register fields are in mram_as3016204.h and mram_as3064204.h, and
              can be sent through Execute() directly.

              Everything here runs in single-lane SPI (1-1-1). Nothing in the API switches the chip
              into another interface mode.

              Not handled here: locking between tasks, keeping execute-in-place running while the
              bus is in use in the flight image, and stopping writes into the code region. Those
              belong to the storage layer.

  @author     Viola Case
  @date       23.09.2026
  @copyright  © Viola Case, 2026. All rights reserved.

**/
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <OwlSat/mram_config.h>
#include <OwlSat/mram_core.h>

namespace OwlSat::MRAM {

  /*!
   * Outcome of a driver call.
   */
  enum struct Result : uint8_t {
    Ok,               //!< Done
    NotSPI,           //!< Execute() refused: BUS_MODE isn't Mode::M1_1_1. Nothing was sent
    WrongDevice,      //!< RDID didn't match Config::MRAMPart
    LatencyUnknown,   //!< RDAR reads couldn't be made consistent with the no-latency reads (AS3064204)
    ConfigMismatch,   //!< A register differs from the flight configuration. The driver still works
    WriteNotVerified, //!< A register read back differently from what was written. Usually a protection bit
    OutOfRange,       //!< The address range runs past the end of the array
  };

  /*!
   * @brief   Names a Result, for logs.
   * @param   result  The result to name
   * @return  The enumerator's name, e.g. "WriteNotVerified"
   */
  const char *ResultName(Result result);

  /*!
   * @brief   Brings the driver up and checks the chip.
   * @details In order:
   *          1. Waits until tPU has passed since boot.
   *          2. Reads RDID and checks it against Config::MRAMPart.
   *          3. Establishes the RDAR latency. On the AS3016204 it's fixed, and Init() reads CR2 to
   *             learn MLATS. On the AS3064204 RDAR's latency *is* CR2.MLATS, which is nonvolatile and
   *             so unknown at boot. Init() compares RDAR reads of SR and FSR with RDSR and RDFSR,
   *             which have no latency. If they disagree, it writes the flight CR2 value (writes need
   *             no latency) and checks again. **This is the only register Init() ever writes.**
   *          4. Runs VerifyConfig().
   * @return  Result::Ok, or Result::ConfigMismatch if the chip works but isn't configured as
   *          Config::MRAMActiveSettings::FLIGHT_DEFAULTS says. Any other result means the driver
   *          can't be used.
   */
  Result Init();

  /*!
   * @brief   Compares the chip's registers with Config::MRAMActiveSettings::FLIGHT_DEFAULTS. Writes
   *          nothing.
   * @return  True if every register matches in the bits its entry's mask covers.
   */
  bool VerifyConfig();

  /*!
   * @brief   Writes every register in Config::MRAMActiveSettings::FLIGHT_DEFAULTS with
   *          WriteRegister(), verifying each one.
   * @return  True if every write was read back correctly.
   */
  bool ApplyConfig();

  /*!
   * @brief   Reads the 4-byte device ID (RDID 9Fh).
   * @param   id  Receives the ID, manufacturer byte first
   * @return  False if Execute() refused
   */
  bool ReadID(std::span<uint8_t, 4> id);

  /*!
   * @brief   Checks an RDID reply against Config::MRAMPart's expected ID, ignoring the bits its
   *          ID_MASK leaves out.
   * @param   id  An RDID reply
   * @return  Whether it identifies the configured part
   */
  bool IsExpectedID(std::span<const uint8_t, 4> id);

  /*!
   * @brief   Reads one register by address (RDAR 65h).
   * @details On the AS3064204 the result is only right if the driver's copy of CR2.MLATS matches the
   *          chip, which Init() makes sure of.
   * @param   address  Register address, e.g. Config::MRAMPart::CR1::ADDRESS
   * @return  The register value, or nothing if Execute() refused
   */
  std::optional<uint8_t> ReadRegister(uint32_t address);

  /*!
   * @brief   Writes one register by address (WREN, then WRAR 71h), waits tCS2 and reads it back.
   * @details Bits the part requires to be 1 (CR4[2] on the AS3016204) are set automatically. Writing
   *          CR2 also updates the driver's copy of MLATS.
   *
   *          The chip doesn't report blocked writes. A write blocked by CR1.MAPLK, or by SR.WPEN with
   *          the WP# pin low, just doesn't happen, and shows up here as Result::WriteNotVerified.
   * @param   address      Register address, e.g. Config::MRAMPart::CR1::ADDRESS
   * @param   value        Value to write
   * @param   verify_mask  Bits that must read back as written. Leave read-only bits out
   * @return  Result::Ok, Result::WriteNotVerified or Result::NotSPI
   */
  Result WriteRegister(uint32_t address, uint8_t value, uint8_t verify_mask = 0xFF);

  /*!
   * @brief   Reads from the memory array (READ 03h), split into as many instructions as needed.
   * @details READ has no latency cycles, so it works whatever CR2.MLATS is set to. Its 50 MHz limit
   *          is far above the direct-mode clock.
   * @param   address  First byte to read
   * @param   dest     Receives the data. Its size is the read length
   * @return  Result::Ok, Result::OutOfRange (nothing is read) or Result::NotSPI
   */
  Result Read(uint32_t address, std::span<uint8_t> dest);

  /*!
   * @brief   Writes to the memory array (WREN, then WRTE 02h), split into as many instructions as
   *          needed. Every chunk gets its own WREN, which works in every CR4/CR1 WRENS mode.
   * @details The data is **not** read back. A write into a block-protected range (SR.TBSEL/BPSEL) is
   *          silently ignored by the chip. Read() the range back if it matters.
   * @param   address  First byte to write
   * @param   src      Data to write. Its size is the write length
   * @return  Result::Ok, Result::OutOfRange (nothing is written) or Result::NotSPI
   */
  Result Write(uint32_t address, std::span<const uint8_t> src);

} // namespace OwlSat::MRAM
