/**

  @file       gnss.cpp
  @brief      GNSS receiver driver implementation. See gnss.h.
  @details    Bytes are read straight from UARTDR rather than through uart_getc(), because
              uart_getc() discards bits 8-11 (FE, PE, BE, OE). Without them, a dropped byte would
              only show up as a failed checksum, with no hint of why.

              Message layouts are from SkyTraq application note AN0037.

  @author     Viola Case
  @date       16.11.2026
  @copyright  © Viola Case, 2026. All rights reserved.

**/

#include <OwlSat/gnss.h>

#include <hardware/gpio.h>
#include <hardware/uart.h>
#include <pico/time.h>

namespace OwlSat::GNSS {

  namespace {

    namespace Cfg = Config::GNSS;

    bool g_initialised = false; //!< Set once Init() has succeeded

    /// UARTDR error bits: overrun, break, parity, framing
    constexpr uint32_t DR_ERROR_BITS = UART_UARTDR_OE_BITS | UART_UARTDR_BE_BITS | UART_UARTDR_PE_BITS | UART_UARTDR_FE_BITS;

    constexpr uint8_t START_0 = 0xA0, START_1 = 0xA1; //!< Start of sequence
    constexpr uint8_t END_0 = 0x0D, END_1 = 0x0A;     //!< End of sequence

    constexpr uint8_t ID_CONFIGURE_MESSAGE_TYPE = 0x09;
    constexpr uint8_t MESSAGE_TYPE_BINARY = 0x02;
    constexpr uint8_t ID_SUB_ID_GROUP = 0x64;         //!< Message ID shared by the 0x64/xx commands
    constexpr uint8_t SID_CONFIGURE_NAV_INTERVAL = 0x2F;
    constexpr uint8_t ATTRIBUTE_SRAM_ONLY = 0x00;     //!< Don't write the receiver's flash
    constexpr uint8_t ID_ACK = 0x83, ID_NACK = 0x84;

    /// The longest answer Configure() waits for: an ACK or NACK naming a message ID and a sub-ID
    constexpr size_t MAX_ACK_FRAME = 3 + FRAME_OVERHEAD;

    /*!
     * @brief   Waits for one received byte, up to a deadline.
     * @param   deadline  Absolute time to give up at
     * @param   dr        Receives the whole UARTDR word: data in bits 0-7, errors in bits 8-11
     * @return  False if the deadline passed with the FIFO empty
     */
    bool NextWord(absolute_time_t deadline, uint32_t &dr) {
      uart_hw_t *hw = uart_get_hw(OWLSAT_GNSS_UART);
      while (!uart_is_readable(OWLSAT_GNSS_UART)) {
        if (time_reached(deadline)) return false;
        tight_loop_contents();
      }
      dr = hw->dr;
      return true;
    }

    /// Empties the RX FIFO and clears the latched error flags
    void Flush() {
      uart_hw_t *hw = uart_get_hw(OWLSAT_GNSS_UART);
      while (uart_is_readable(OWLSAT_GNSS_UART))
        (void)hw->dr;
      hw->rsr = 0; // any write clears FE/PE/BE/OE
    }

    /// 8-bit XOR of the bytes, the AN0037 checksum
    uint8_t Checksum(std::span<const uint8_t> bytes) {
      uint8_t sum = 0;
      for (const uint8_t byte : bytes)
        sum ^= byte;
      return sum;
    }

    /*!
     * @brief   Reads the next frame that fits in a buffer, without validating it.
     * @details Scans for A0 A1, which skips NMEA and line noise. Line errors during the scan don't
     *          matter, those bytes are thrown away anyway. A frame whose length field says it won't
     *          fit isn't one the caller wants, so the scan simply carries on through it.
     * @param   deadline  Absolute time to give up at
     * @param   buffer    Receives the frame, start sequence included
     * @param   length    Receives the frame's total length
     * @return  Result::Ok with a frame in the buffer, or NoMessage, Incomplete or LineError
     */
    Result NextFrame(absolute_time_t deadline, std::span<uint8_t> buffer, size_t &length) {
      uint32_t dr = 0;
      for (;;) {
        // Sync
        uint8_t previous = 0;
        for (;;) {
          if (!NextWord(deadline, dr)) return Result::NoMessage;
          const uint8_t byte = (dr & DR_ERROR_BITS) != 0 ? 0 : static_cast<uint8_t>(dr & UART_UARTDR_DATA_BITS);
          if (previous == START_0 && byte == START_1) break;
          previous = byte;
        }
        buffer[0] = START_0;
        buffer[1] = START_1;

        // The rest: length field first, then as many bytes as it asks for
        size_t total = PAYLOAD_OFFSET;
        bool fits = true;
        for (size_t i = 2; i < total; ++i) {
          if (!NextWord(deadline, dr)) return Result::Incomplete;
          if ((dr & DR_ERROR_BITS) != 0) return Result::LineError;
          buffer[i] = static_cast<uint8_t>(dr & UART_UARTDR_DATA_BITS);
          if (i == PAYLOAD_OFFSET - 1) {
            const size_t payload_length = (static_cast<size_t>(buffer[2]) << 8) | buffer[3];
            if (payload_length == 0 || payload_length + FRAME_OVERHEAD > buffer.size()) {
              fits = false;
              break;
            }
            total = payload_length + FRAME_OVERHEAD;
          }
        }
        if (!fits) continue;

        length = total;
        return Result::Ok;
      }
    }

    /*!
     * @brief   Sends one command frame. Drives the TX pin for the duration, then releases it.
     * @param   payload  Message ID, sub-ID if any, and message body
     */
    void Send(std::span<const uint8_t> payload) {
      const uint8_t header[] = {START_0, START_1, static_cast<uint8_t>(payload.size() >> 8), static_cast<uint8_t>(payload.size())};
      const uint8_t trailer[] = {Checksum(payload), END_0, END_1};

      gpio_set_function(Cfg::UART_TX, GPIO_FUNC_UART);
      uart_write_blocking(OWLSAT_GNSS_UART, header, sizeof header);
      uart_write_blocking(OWLSAT_GNSS_UART, payload.data(), payload.size());
      uart_write_blocking(OWLSAT_GNSS_UART, trailer, sizeof trailer);
      uart_tx_wait_blocking(OWLSAT_GNSS_UART);  // the stop bit of the last byte has to go out first
      gpio_set_function(Cfg::UART_TX, GPIO_FUNC_SIO); // back to the pulled-up input Init() left
    }

    /*!
     * @brief   Sends a command and waits for the receiver's ACK or NACK to it.
     * @param   payload     Message ID, sub-ID if any, and message body
     * @param   has_sub_id  Whether payload[1] is a sub-ID, which the ACK then repeats
     * @return  Result::Ok on ACK, Result::Rejected on NACK, otherwise why no answer arrived
     */
    Result Command(std::span<const uint8_t> payload, bool has_sub_id) {
      Flush();
      Send(payload);

      const absolute_time_t deadline = make_timeout_time_ms(Cfg::ACK_TIMEOUT_MS);
      std::array<uint8_t, MAX_ACK_FRAME> frame;
      for (;;) {
        size_t length = 0;
        const Result result = NextFrame(deadline, frame, length);
        if (result != Result::Ok) return result;
        if (!IsValidFrame(std::span(frame).first(length))) continue;

        const uint8_t id = frame[PAYLOAD_OFFSET];
        const size_t payload_length = length - FRAME_OVERHEAD;
        if ((id != ID_ACK && id != ID_NACK) || payload_length < 2 || frame[PAYLOAD_OFFSET + 1] != payload[0]) continue;
        // The receiver may or may not repeat the sub-ID. If it does, it has to be ours
        if (has_sub_id && payload_length >= 3 && frame[PAYLOAD_OFFSET + 2] != payload[1]) continue;
        return id == ID_ACK ? Result::Ok : Result::Rejected;
      }
    }

  } // namespace


  const char *ResultName(Result result) {
    switch (result) {
    case Result::Ok:
      return "Ok";
    case Result::NotInitialised:
      return "NotInitialised";
    case Result::NoMessage:
      return "NoMessage";
    case Result::Incomplete:
      return "Incomplete";
    case Result::LineError:
      return "LineError";
    case Result::BadFrame:
      return "BadFrame";
    case Result::Rejected:
      return "Rejected";
    }
    return "?";
  }


  bool IsValidFrame(std::span<const uint8_t> frame) {
    if (frame.size() <= FRAME_OVERHEAD) return false;
    const size_t payload_length = frame.size() - FRAME_OVERHEAD;
    if (frame[0] != START_0 || frame[1] != START_1) return false;
    if (frame[2] != static_cast<uint8_t>(payload_length >> 8) || frame[3] != static_cast<uint8_t>(payload_length)) return false;
    if (frame[frame.size() - 2] != END_0 || frame[frame.size() - 1] != END_1) return false;
    return Checksum(frame.subspan(PAYLOAD_OFFSET, payload_length)) == frame[frame.size() - 3];
  }


  bool Init() {
    if constexpr (!Pins::IsAssigned(Cfg::UART_RX) || !Pins::IsAssigned(Cfg::UART_TX)) {
      return false;
    }
    uart_init(OWLSAT_GNSS_UART, Cfg::BAUD); // 8N1, FIFOs on
    uart_set_hw_flow(OWLSAT_GNSS_UART, false, false);
    gpio_set_function(Cfg::UART_RX, GPIO_FUNC_UART);

    // TX is a pulled-up input until Send() borrows it. See Config::GNSS::UART_TX.
    gpio_init(Cfg::UART_TX);
    gpio_pull_up(Cfg::UART_TX);

    Flush();
    g_initialised = true;
    return true;
  }


  Result Configure() {
    if (!g_initialised) return Result::NotInitialised;

    const uint8_t message_type[] = {ID_CONFIGURE_MESSAGE_TYPE, MESSAGE_TYPE_BINARY, ATTRIBUTE_SRAM_ONLY};
    if (const Result result = Command(message_type, false); result != Result::Ok) return result;

    const uint8_t nav_interval[] = {ID_SUB_ID_GROUP, SID_CONFIGURE_NAV_INTERVAL, Cfg::MESSAGE_INTERVAL_S, ATTRIBUTE_SRAM_ONLY};
    return Command(nav_interval, true);
  }


  Result Read(Frame &out) {
    if (!g_initialised) return Result::NotInitialised;

    const absolute_time_t deadline = make_timeout_time_ms(Cfg::READ_TIMEOUT_MS);
    Flush();

    // Fill a local buffer, so out is only written if the whole frame is good
    Frame frame;
    for (;;) {
      size_t length = 0;
      const Result result = NextFrame(deadline, frame, length);
      if (result != Result::Ok) return result;
      if (length != frame.size() || frame[PAYLOAD_OFFSET] != Cfg::MESSAGE_ID) continue; // some other message

      if (!IsValidFrame(frame)) return Result::BadFrame;
      out = frame;
      return Result::Ok;
    }
  }

} // namespace OwlSat::GNSS
