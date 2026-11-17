/**

  @file       gnss.h
  @brief      GNSS receiver driver: one fixed-length binary navigation message from the Orion
              B16-C over UART1.
  @details    The Orion powers up streaming NMEA-0183. Configure() switches it to the SkyTraq
              binary protocol (application note AN0037), and Read() then hands back one Navigation
              Data Message (ID 0xA8) as a whole frame. That frame is always the same length:

              @code
              A0 A1 | PL (2, big-endian) | payload (PL bytes, message ID first) | CS | 0D 0A
              @endcode

              CS is the XOR of the payload bytes. For 0xA8, PL is 59, so the frame is 66 bytes.
              Read() checks the framing and the checksum but does not decode any fields. Multi-byte
              fields inside the payload are big-endian.

              Configure() writes the receiver's SRAM only, never its flash, so the receiver is back
              to NMEA after a power cycle and Configure() has to be called again.

              Not handled here: powering the receiver (GPS_PWR) and sharing the UART between tasks.

              @par Threading and timing
              Pure pico-sdk with no FreeRTOS dependency. Read() polls the UART until a deadline, so
              it occupies its caller for up to Config::GNSS::READ_TIMEOUT_MS, and Configure() for up
              to two Config::GNSS::ACK_TIMEOUT_MS. The RX FIFO holds 32 bytes, about 2.7 ms at
              115200. If the caller is preempted for longer than that partway through a frame, the
              FIFO overruns and the call returns Result::LineError rather than a frame with bytes
              missing. Call both from one task only, never from an ISR.

  @author     Viola Case
  @date       16.11.2026
  @copyright  © Viola Case, 2026. All rights reserved.

**/
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <OwlSat/gnss_config.h>

namespace OwlSat::GNSS {

  /*!
   * Outcome of Configure() and Read().
   */
  enum struct Result : uint8_t {
    Ok,             //!< Read(): the frame is complete and its checksum matches. Configure(): every command was ACKed
    NotInitialised, //!< Init() hasn't succeeded
    NoMessage,      //!< Nothing usable before the deadline: no navigation message (Read) or no ACK/NACK (Configure)
    Incomplete,     //!< A frame started, but didn't finish before the deadline
    LineError,      //!< Framing, parity, break or overrun error inside a frame. Bytes may be missing
    BadFrame,       //!< The navigation message arrived with a wrong checksum or end sequence
    Rejected,       //!< Configure(): the receiver answered a command with NACK
  };

  /*!
   * @brief   Names a Result, for logs.
   * @param   result  The result to name
   * @return  The enumerator's name, e.g. "NoMessage"
   */
  const char *ResultName(Result result);

  inline constexpr size_t FRAME_OVERHEAD = 7; //!< A0 A1, two length bytes, checksum, 0D 0A
  inline constexpr size_t PAYLOAD_OFFSET = 4; //!< Index of the message ID, the first payload byte, in a frame
  inline constexpr size_t FRAME_LENGTH = Config::GNSS::PAYLOAD_LENGTH + FRAME_OVERHEAD;

  /// One whole navigation message frame, from the A0 A1 start sequence to the 0D 0A end sequence
  using Frame = std::array<uint8_t, FRAME_LENGTH>;

  /*!
   * @brief   Brings up OWLSAT_GNSS_UART at Config::GNSS::BAUD, 8N1, and muxes the RX pin.
   * @details Sends nothing. The TX pin becomes a pulled-up input (see Config::GNSS::UART_TX).
   *          Can't tell whether a receiver is there. Configure() does that.
   * @return  False if Config::GNSS::UART_RX or UART_TX is unassigned.
   */
  bool Init();

  /*!
   * @brief   Switches the receiver to binary output and turns the navigation message on.
   * @details Sends two AN0037 commands and waits for the ACK to each:
   *          1. Configure Message Type (0x09): binary.
   *          2. Configure Navigation Data Message Interval (0x64 / 0x2F): Config::GNSS::MESSAGE_INTERVAL_S.
   *          Both update SRAM only. Drives the TX pin while sending, so **call it only while the
   *          receiver is powered**, and again after every receiver power cycle.
   * @return  Result::Ok if both were ACKed. Result::Rejected on a NACK. Otherwise why no answer arrived
   */
  Result Configure();

  /*!
   * @brief   Reads the next navigation message.
   * @details 1. Throws away whatever is in the RX FIFO and clears the error flags. It's stale and
   *             has probably overrun.
   *          2. Waits for the next frame carrying Config::GNSS::MESSAGE_ID, skipping every other
   *             message and any NMEA.
   *          3. Checks its end sequence and checksum.
   *          All of it runs against one deadline, Config::GNSS::READ_TIMEOUT_MS from the call.
   * @param   out  Filled only on Result::Ok, untouched otherwise
   * @return  Result::Ok, or why there's no frame
   */
  Result Read(Frame &out);

  /*!
   * @brief   Checks a whole binary frame: start sequence, length field, checksum and end sequence.
   * @details Pure, touches no hardware. Read() uses it on every frame.
   * @param   frame  The frame, A0 A1 through 0D 0A, with nothing before or after
   * @return  True if the frame is well formed
   */
  bool IsValidFrame(std::span<const uint8_t> frame);

} // namespace OwlSat::GNSS
