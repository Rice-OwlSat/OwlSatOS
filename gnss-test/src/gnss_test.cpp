/**

    @file      gnss_test.cpp
    @brief     Bench test for the GNSS driver (gnss.h) against the Orion B16-C.
    @details   Prints over UART0 (GP0 TX / GP1 RX, 115200 8N1) to the debug probe. Runs from SRAM
               (no_flash). Debug builds also carry the USB reset interface, so tools/reboot.ps1 can
               restart or reload the board without the BOOTSEL button.

               At start-up it runs a self-check that needs no receiver: GNSS::IsValidFrame() must
               accept the example Navigation Data Message printed in AN0037 and reject a copy with
               one bit flipped. Then it calls GNSS::Configure() to switch the receiver to binary.

               Every Config::GNSSTest::PERIOD_MS it calls GNSS::Read() and prints the result and how
               long the call took. On Ok it also prints a hex dump of the frame and a few decoded
               fields, so a person can see the fix is plausible. A running tally follows each read.
               After a failed read it calls Configure() again, so a receiver that is plugged in or
               power-cycled while the test runs gets switched to binary without a restart. That
               pulses the TX pin for about a millisecond per attempt even with no receiver powered,
               which is fine on the bench but is not how flight code should call Configure().

               With no receiver wired, the self-check passes, Configure() ends in NoMessage, and
               every read ends in NoMessage after Config::GNSS::READ_TIMEOUT_MS.

               @par Bench results, 15.11.2026 (Pico 2, no Orion attached, NMEA version of the driver)
               41 consecutive reads, all timing out at exactly 2500 ms, tally counting correctly.
               That confirmed the harness, the UART0 console to the debug probe, Init(), and the
               timeout path. The driver has since been rewritten for the binary protocol.

               @par Still unconfirmed
               Nothing here has been run against a real receiver. That needs the Orion wired to
               UART1 (its TXD to Config::GNSS::UART_RX, its RXD to Config::GNSS::UART_TX). Expect
               "Configure(): Ok", then Ok reads with a hex dump starting A0 A1 00 3B A8. Open points
               only the hardware can settle: whether this receiver ACKs Configure Message Type
               (0x09) and Configure Navigation Data Message Interval (0x64/0x2F), and whether it
               then sends 0xA8.

    @author    Viola Case
    @date      16.11.2026
    @copyright © Viola Case, 2026. All right reserved.

**/

#include <cstdio>
#include <span>

#include <pico/stdlib.h>

#include <OwlSat/gnss.h>

namespace {

  using namespace OwlSat;
  namespace Cfg = Config::GNSS;
  namespace Test = Config::GNSSTest;

  /// The example Navigation Data Message from AN0037 (0xA8), whole frame
  constexpr uint8_t AN0037_EXAMPLE[] = {
      0xA0, 0xA1, 0x00, 0x3B, 0xA8, 0x02, 0x08, 0x06, 0x04, 0x02, 0x32, 0x18, 0x18, 0x0E, 0xC5, 0xE1, 0x99, 0x48, 0x20, 0x78, 0xED, 0x00,
      0x00, 0x2E, 0x3B, 0x00, 0x00, 0x26, 0x93, 0x00, 0x93, 0x00, 0x93, 0x00, 0x93, 0x00, 0x93, 0x00, 0x93, 0xEE, 0x35, 0x4D, 0x30, 0x1D,
      0x99, 0xAA, 0x37, 0x0F, 0xD7, 0x0B, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF5, 0x0D, 0x0A,
  };
  static_assert(sizeof AN0037_EXAMPLE == GNSS::FRAME_LENGTH, "The AN0037 example is one whole 0xA8 frame");

  /*!
   * @brief   Prints bytes as hex, 16 per line.
   * @param   bytes  The bytes
   */
  void HexDump(std::span<const uint8_t> bytes) {
    for (size_t row = 0; row < bytes.size(); row += 16) {
      printf("  %04X: ", static_cast<unsigned>(row));
      for (size_t i = row; i < row + 16 && i < bytes.size(); ++i)
        printf(" %02X", bytes[i]);
      printf("\n");
    }
  }

  /// @return The big-endian unsigned value of `count` bytes at payload field `field` (1 = message ID, as numbered in AN0037)
  uint32_t Field(std::span<const uint8_t> frame, size_t field, size_t count) {
    uint32_t value = 0;
    for (size_t i = 0; i < count; ++i)
      value = (value << 8) | frame[GNSS::PAYLOAD_OFFSET + field - 1 + i];
    return value;
  }

  /*!
   * @brief   Prints a few fields of a Navigation Data Message, for a person to sanity-check.
   * @param   frame  A whole, valid 0xA8 frame
   */
  void PrintNavigation(std::span<const uint8_t> frame) {
    static const char *const FIX_MODES[] = {"no fix", "2D", "3D", "3D+DGNSS"};
    const uint32_t fix = Field(frame, 2, 1);
    printf("  fix %lu (%s), %lu SV, week %lu, TOW %.2f s\n", static_cast<unsigned long>(fix), fix < 4 ? FIX_MODES[fix] : "?",
           static_cast<unsigned long>(Field(frame, 3, 1)), static_cast<unsigned long>(Field(frame, 4, 2)), Field(frame, 6, 4) / 100.0);
    printf("  lat %.7f deg, lon %.7f deg, ellipsoid alt %.2f m, MSL alt %.2f m\n", static_cast<int32_t>(Field(frame, 10, 4)) / 1e7,
           static_cast<int32_t>(Field(frame, 14, 4)) / 1e7, static_cast<int32_t>(Field(frame, 18, 4)) / 100.0, static_cast<int32_t>(Field(frame, 22, 4)) / 100.0);
  }

  /*!
   * @brief   Checks GNSS::IsValidFrame() against AN0037's example frame. Needs no receiver.
   * @return  True if the example is accepted and a corrupted copy is rejected
   */
  bool SelfCheck() {
    GNSS::Frame corrupted;
    for (size_t i = 0; i < corrupted.size(); ++i)
      corrupted[i] = AN0037_EXAMPLE[i];
    corrupted[20] ^= 0x01;

    const bool accepts = GNSS::IsValidFrame(AN0037_EXAMPLE);
    const bool rejects = !GNSS::IsValidFrame(corrupted);
    printf("[%s] IsValidFrame() accepts the AN0037 example frame\n", accepts ? "PASS" : "FAIL");
    printf("[%s] IsValidFrame() rejects it with one bit flipped\n", rejects ? "PASS" : "FAIL");
    if (accepts) PrintNavigation(AN0037_EXAMPLE);
    return accepts && rejects;
  }

  /// Calls GNSS::Configure() and prints how it went
  void Configure() {
    const absolute_time_t start = get_absolute_time();
    const GNSS::Result result = GNSS::Configure();
    const int64_t took_ms = absolute_time_diff_us(start, get_absolute_time()) / 1000;
    printf("[%s] GNSS::Configure(): %s (%lld ms)\n", result == GNSS::Result::Ok ? "PASS" : "FAIL", GNSS::ResultName(result), static_cast<long long>(took_ms));
  }

} // namespace


int main() {
  stdio_init_all();
  sleep_ms(Test::STARTUP_DELAY_MS);

  printf("\n=== GNSS driver bench test (Orion B16-C, SkyTraq binary) ===\n");
  printf("UART1 RX GP%d, TX GP%d (driven only while configuring), %lu baud 8N1\n", Cfg::UART_RX, Cfg::UART_TX, static_cast<unsigned long>(Cfg::BAUD));
  printf("Message 0x%02X, frame %u bytes, every %u s, read timeout %lu ms, period %lu ms\n", Cfg::MESSAGE_ID, static_cast<unsigned>(GNSS::FRAME_LENGTH),
         Cfg::MESSAGE_INTERVAL_S, static_cast<unsigned long>(Cfg::READ_TIMEOUT_MS), static_cast<unsigned long>(Test::PERIOD_MS));

  SelfCheck();

  if (!GNSS::Init()) {
    printf("[FAIL] GNSS::Init(): a UART pin is unassigned in gnss_config.h. Halting.\n");
    for (;;)
      sleep_ms(1000); // not a tight loop, so the USB reset interface keeps answering
  }
  printf("[INFO] GNSS::Init() done\n");
  Configure();

  unsigned reads = 0, ok = 0, failed = 0;
  GNSS::Frame frame {};
  for (;;) {
    ++reads;
    const absolute_time_t start = get_absolute_time();
    const GNSS::Result result = GNSS::Read(frame);
    const int64_t took_ms = absolute_time_diff_us(start, get_absolute_time()) / 1000;

    printf("\n--- read %u: %s (%lld ms) ---\n", reads, GNSS::ResultName(result), static_cast<long long>(took_ms));
    if (result == GNSS::Result::Ok) {
      ++ok;
      HexDump(frame);
      PrintNavigation(frame);
    } else {
      ++failed;
      Configure(); // the receiver may have just been plugged in or power-cycled
    }
    printf("tally: %u reads, %u Ok, %u failed\n", reads, ok, failed);

    sleep_ms(Test::PERIOD_MS);
  }
}
