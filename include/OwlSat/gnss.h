/**

  @file       gnss.h
  @brief      GNSS receiver driver: fixed-length NMEA chunks from the Orion B16-C over UART1.
  @details    The Orion streams NMEA-0183 sentences ($aaccc,...*hh<CR><LF>) at 115200 8N1 with
              no configuration. This driver does one thing. It hands back Config::GNSS::READ_LENGTH
              raw bytes that start at the beginning of a sentence. It does not parse anything, and
              it never sends anything to the receiver.

              Not handled here: powering the receiver (GPS_PWR), sharing the UART between tasks,
              and SkyTraq binary commands.

              @par Threading and timing
              Pure pico-sdk with no FreeRTOS dependency. Read() polls the UART until a deadline, so
              it occupies its caller for up to Config::GNSS::READ_TIMEOUT_MS. The RX FIFO holds 32
              bytes, about 2.7 ms at 115200. If the caller is preempted for longer than that partway
              through a chunk, the FIFO overruns and Read() returns Result::LineError rather than a
              chunk with bytes missing. Call it from one task only, never from an ISR.

  @author     Viola Case
  @date       15.11.2026
  @copyright  © Viola Case, 2026. All rights reserved.

**/
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

#include <OwlSat/gnss_config.h>

namespace OwlSat::GNSS {

  /*!
   * Outcome of Read().
   */
  enum struct Result : uint8_t {
    Ok,             //!< The chunk is full and starts with '$'
    NotInitialised, //!< Init() hasn't succeeded
    NoSentence,     //!< No '$' arrived before the deadline. The receiver is silent, unpowered or not wired
    Incomplete,     //!< Synced on '$', but the chunk didn't fill before the deadline
    LineError,      //!< Framing, parity, break or overrun error after sync. Bytes may be missing
  };

  /*!
   * @brief   Names a Result, for logs.
   * @param   result  The result to name
   * @return  The enumerator's name, e.g. "NoSentence"
   */
  const char *ResultName(Result result);

  /// One chunk: READ_LENGTH raw bytes, starting with '$'. Not NUL-terminated.
  using Chunk = std::array<uint8_t, Config::GNSS::READ_LENGTH>;

  /*!
   * @brief   Brings up OWLSAT_GNSS_UART at Config::GNSS::BAUD, 8N1, and muxes only the RX pin.
   * @details Doesn't touch the TX pin. See Config::GNSS::UART_TX for why. Can't tell whether a
   *          receiver is there, because that takes listening, which Read() does.
   * @return  False if Config::GNSS::UART_RX is unassigned.
   */
  bool Init();

  /*!
   * @brief   Reads one chunk of NMEA output.
   * @details 1. Throws away whatever is in the RX FIFO and clears the error flags. It's stale and
   *             has probably overrun.
   *          2. Waits for the next '$'.
   *          3. Fills the rest of the chunk with the bytes that follow.
   *          All of it runs against one deadline, Config::GNSS::READ_TIMEOUT_MS from the call.
   * @param   out  Filled only on Result::Ok, untouched otherwise
   * @return  Result::Ok, or why there's no chunk
   */
  Result Read(Chunk &out);

} // namespace OwlSat::GNSS
