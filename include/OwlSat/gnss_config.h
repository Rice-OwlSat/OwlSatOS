/**

  @file       gnss_config.h
  @brief      Every editable GNSS value: the UART1 pins, line settings, the binary message that is
              read, timeouts and bench test parameters.
  @details    The receiver is the SkyTraq Orion B16-C. Out of the box it streams NMEA-0183 at
              115200 8N1 (datasheet p.2, p.10). The driver switches it to the SkyTraq binary
              protocol (application note AN0037) and reads one fixed-length message from it.

              The pins live here rather than in pin_assignment.h because the GNSS harness is not
              wired yet. When the board pinout is settled they move into OwlSat::Pins with the rest.

  @author     Viola Case
  @date       16.11.2026
  @copyright  © Viola Case, 2026. All rights reserved.

**/
#pragma once
#include <cstddef>
#include <cstdint>

#include <OwlSat/pin_assignment.h>

/// UART instance wired to the Orion. A macro for the same reason OWLSAT_UART0 is (see pin_assignment.h).
#define OWLSAT_GNSS_UART OWLSAT_UART1

namespace OwlSat::Config {

  namespace GNSS {

    /*!
     * UART1 pins. **Placeholders (TBC)**: the Orion isn't wired yet, and GP4/GP5 currently
     * overlap the I2C0 placeholders in pin_assignment.h. That doesn't matter on the bench, but the
     * pinout has to be sorted out before both run together.
     *
     * UART_TX is only driven while GNSS::Configure() sends a command. The Orion's RXD must not be
     * driven high while the module is asleep or unpowered (datasheet p.5), and a UART TX line idles
     * high. Between commands the pin is an input with the RP2350's internal pull-up, which keeps
     * the line at its idle level through far more than the 10 kOhm the datasheet allows.
     */
    inline constexpr Pins::Pin UART_TX = 4; //!< FC to Orion RXD (pin 21). Driven only inside Configure()
    inline constexpr Pins::Pin UART_RX = 5; //!< Orion TXD (pin 20) to FC

    inline constexpr uint32_t BAUD = 115200; //!< Orion default, 8N1 (datasheet p.2)

    /*!
     * The binary message GNSS::Read() returns: Navigation Data Message, ID 0xA8 (AN0037). Its
     * payload is always 59 bytes, the message ID included, so the whole frame is always 66 bytes.
     */
    inline constexpr uint8_t MESSAGE_ID = 0xA8;
    inline constexpr size_t PAYLOAD_LENGTH = 59; //!< Payload bytes of MESSAGE_ID, counting the ID byte

    /// Seconds between navigation data messages, 1-255 (AN0037, ID 0x64 / SID 0x2F)
    inline constexpr uint8_t MESSAGE_INTERVAL_S = 1;

    /*!
     * Upper bound on one GNSS::Read() [ms], covering both the wait for the message and reading it.
     * Keep it comfortably above MESSAGE_INTERVAL_S.
     */
    inline constexpr uint32_t READ_TIMEOUT_MS = 2500;

    /// How long GNSS::Configure() waits for the ACK or NACK to each command [ms]
    inline constexpr uint32_t ACK_TIMEOUT_MS = 1000;

    // GPIO -> UART1 function mapping (RP2350 datasheet, GPIO function table, F2 = UART)
    static_assert(!Pins::IsAssigned(UART_TX) || UART_TX == 4 || UART_TX == 8 || UART_TX == 20 || UART_TX == 24,
                  "UART1 TX is only available on GP4, GP8, GP20 or GP24");
    static_assert(!Pins::IsAssigned(UART_RX) || UART_RX == 5 || UART_RX == 9 || UART_RX == 21 || UART_RX == 25,
                  "UART1 RX is only available on GP5, GP9, GP21 or GP25");
    static_assert(PAYLOAD_LENGTH >= 1 && PAYLOAD_LENGTH <= 0xFFFF, "The payload holds at least the message ID, and its length is 16 bits");
    static_assert(MESSAGE_INTERVAL_S >= 1, "An interval of 0 turns the message off");
    static_assert(READ_TIMEOUT_MS > MESSAGE_INTERVAL_S * 1000u, "A read has to span at least one message interval");

  } // namespace GNSS

  namespace GNSSTest {
    inline constexpr uint32_t STARTUP_DELAY_MS = 2000; //!< Pause before the first line, so a serial reader can attach
    inline constexpr uint32_t PERIOD_MS = 1000;        //!< Gap between reads
  } // namespace GNSSTest

} // namespace OwlSat::Config
