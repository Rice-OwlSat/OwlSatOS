/**

  @file       gnss_config.h
  @brief      Every editable GNSS value: the UART1 pins, line settings, chunk length and bench test
              parameters.
  @details    The receiver is the SkyTraq Orion B16-C. Out of the box it streams NMEA-0183 at
              115200 8N1 on its TXD pin, once per second, with no configuration needed
              (datasheet p.2, p.10).

              The pins live here rather than in pin_assignment.h because the GNSS harness is not
              wired yet. When the board pinout is settled they move into OwlSat::Pins with the rest.

  @author     Viola Case
  @date       15.11.2026
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
     * The driver never muxes UART_TX. The Orion's RXD must not be driven high while the module is
     * unpowered (datasheet p.5), and a UART TX line idles high. The driver sends no commands, so
     * the pin stays a high-Z input. It's listed here so the pin is reserved.
     */
    inline constexpr Pins::Pin UART_TX = 4; //!< FC to Orion RXD (pin 21). Reserved, not driven
    inline constexpr Pins::Pin UART_RX = 5; //!< Orion TXD (pin 20) to FC

    inline constexpr uint32_t BAUD = 115200; //!< Orion default, 8N1 (datasheet p.2)

    /*!
     * Bytes returned by one GNSS::Read(). The chunk starts on a '$' and is cut off at this length,
     * usually in the middle of a sentence. Placeholder. At the default 1 Hz the Orion sends several
     * hundred bytes per burst, so 256 always fits inside one burst.
     */
    inline constexpr size_t READ_LENGTH = 256;

    /*!
     * Upper bound on one GNSS::Read() [ms], covering both the wait for '$' and the fill. Output
     * comes in one burst per second, so a read that starts just after a burst can wait almost a
     * second for the next '$'. Raise this if READ_LENGTH grows past one burst.
     */
    inline constexpr uint32_t READ_TIMEOUT_MS = 2500;

    // GPIO -> UART1 function mapping (RP2350 datasheet, GPIO function table, F2 = UART)
    static_assert(!Pins::IsAssigned(UART_TX) || UART_TX == 4 || UART_TX == 8 || UART_TX == 20 || UART_TX == 24,
                  "UART1 TX is only available on GP4, GP8, GP20 or GP24");
    static_assert(!Pins::IsAssigned(UART_RX) || UART_RX == 5 || UART_RX == 9 || UART_RX == 21 || UART_RX == 25,
                  "UART1 RX is only available on GP5, GP9, GP21 or GP25");
    static_assert(READ_LENGTH >= 1, "A chunk holds at least the '$'");

  } // namespace GNSS

  namespace GNSSTest {
    inline constexpr uint32_t STARTUP_DELAY_MS = 2000; //!< Pause before the first line, so a serial reader can attach
    inline constexpr uint32_t PERIOD_MS = 1000;        //!< Gap between reads
  } // namespace GNSSTest

} // namespace OwlSat::Config
