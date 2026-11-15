/**

  @file       gnss.cpp
  @brief      GNSS receiver driver implementation. See gnss.h.
  @details    Bytes are read straight from UARTDR rather than through uart_getc(), because
              uart_getc() discards bits 8-11 (FE, PE, BE, OE). Without them, a dropped byte would
              silently shift the rest of the chunk.

  @author     Viola Case
  @date       15.11.2026
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

  } // namespace


  const char *ResultName(Result result) {
    switch (result) {
    case Result::Ok:
      return "Ok";
    case Result::NotInitialised:
      return "NotInitialised";
    case Result::NoSentence:
      return "NoSentence";
    case Result::Incomplete:
      return "Incomplete";
    case Result::LineError:
      return "LineError";
    }
    return "?";
  }


  bool Init() {
    if constexpr (!Pins::IsAssigned(Cfg::UART_RX)) {
      return false;
    }
    uart_init(OWLSAT_GNSS_UART, Cfg::BAUD); // 8N1, FIFOs on
    uart_set_hw_flow(OWLSAT_GNSS_UART, false, false);
    gpio_set_function(Cfg::UART_RX, GPIO_FUNC_UART);
    // UART_TX is deliberately left as a high-Z input. See Config::GNSS::UART_TX.

    Flush();
    g_initialised = true;
    return true;
  }


  Result Read(Chunk &out) {
    if (!g_initialised) return Result::NotInitialised;

    const absolute_time_t deadline = make_timeout_time_ms(Cfg::READ_TIMEOUT_MS);
    Flush();

    // Sync. Errors before the '$' don't matter: those bytes are thrown away anyway.
    uint32_t dr = 0;
    do {
      if (!NextWord(deadline, dr)) return Result::NoSentence;
    } while ((dr & DR_ERROR_BITS) != 0 || (dr & UART_UARTDR_DATA_BITS) != '$');

    // Fill a local buffer, so out is only written if the whole chunk is good
    Chunk chunk;
    chunk[0] = '$';
    for (size_t i = 1; i < chunk.size(); ++i) {
      if (!NextWord(deadline, dr)) return Result::Incomplete;
      if ((dr & DR_ERROR_BITS) != 0) return Result::LineError;
      chunk[i] = static_cast<uint8_t>(dr & UART_UARTDR_DATA_BITS);
    }

    out = chunk;
    return Result::Ok;
  }

} // namespace OwlSat::GNSS
