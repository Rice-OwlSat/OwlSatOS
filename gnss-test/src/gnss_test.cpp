/**

    @file      gnss_test.cpp
    @brief     Bench test for the GNSS driver (gnss.h) against the Orion B16-C.
    @details   Prints over UART0 (GP0 TX / GP1 RX, 115200 8N1) to the debug probe. Runs from SRAM
               (no_flash) and never sends anything to the receiver.

               Every Config::GNSSTest::PERIOD_MS it calls GNSS::Read() and prints the result and how
               long the call took. On Ok it also prints a hex + ASCII dump of the chunk and counts
               the complete sentences in it whose NMEA checksum matches. That shows the bytes
               arrived intact without parsing any fields. A running tally follows each read.

               With no receiver wired, every read should end in NoSentence after
               Config::GNSS::READ_TIMEOUT_MS.

               @par Bench results, 15.11.2026 (Pico 2, no Orion attached)
               41 consecutive reads, all NoSentence at exactly 2500 ms, tally counting correctly:
               @code
               --- read 41: NoSentence (2500 ms) ---
               tally: 41 reads, 0 Ok, 41 failed
               @endcode
               This confirms the harness, the UART0 console to the debug probe, Init(), and Read()'s
               timeout path. It does **not** confirm that the driver works: the '$' sync, the
               chunk fill, line-error detection and the checksum check have never seen real NMEA.
               That needs the Orion wired to UART1 (its TXD to Config::GNSS::UART_RX). Expect Ok
               reads with a hex dump and "[PASS] n of n complete sentences have a matching
               checksum".

    @author    Viola Case
    @date      15.11.2026
    @copyright © Viola Case, 2026. All right reserved.

**/

#include <cctype>
#include <cstdio>
#include <span>

#include <pico/stdlib.h>

#include <OwlSat/gnss.h>

namespace {

  using namespace OwlSat;
  namespace Cfg = Config::GNSS;
  namespace Test = Config::GNSSTest;

  /*!
   * @brief   Prints bytes as hex and ASCII, 16 per line. Non-printables (CR, LF, ...) show as '.'.
   * @param   bytes  The bytes
   */
  void HexDump(std::span<const uint8_t> bytes) {
    for (size_t row = 0; row < bytes.size(); row += 16) {
      printf("  %04X: ", static_cast<unsigned>(row));
      for (size_t i = row; i < row + 16; ++i) {
        if (i < bytes.size()) printf(" %02X", bytes[i]);
        else printf("   ");
      }
      printf("  |");
      for (size_t i = row; i < row + 16 && i < bytes.size(); ++i)
        putchar(isprint(bytes[i]) ? bytes[i] : '.');
      printf("|\n");
    }
  }

  /// @return The value of one hex digit, or -1
  int HexValue(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  }

  struct SentenceCount {
    unsigned complete = 0; //!< Sentences with '$', '*' and two hex digits all inside the chunk
    unsigned valid = 0;    //!< ...of which the XOR checksum matches
  };

  /*!
   * @brief   Checks every complete $...*hh sentence in a chunk. A sentence cut off by the end of the
   *          chunk is skipped.
   * @param   bytes  The chunk
   * @return  How many sentences were complete and how many of those had a matching checksum
   */
  SentenceCount CheckSentences(std::span<const uint8_t> bytes) {
    SentenceCount count;
    size_t i = 0;
    while (i < bytes.size()) {
      if (bytes[i] != '$') {
        ++i;
        continue;
      }
      // XOR everything between '$' and '*'
      uint8_t sum = 0;
      size_t j = i + 1;
      while (j < bytes.size() && bytes[j] != '*' && bytes[j] != '$')
        sum ^= bytes[j++];
      if (j + 2 >= bytes.size() || bytes[j] != '*') { // cut off, or a '$' before any '*'
        i = j;
        continue;
      }
      int hi = HexValue(bytes[j + 1]), lo = HexValue(bytes[j + 2]);
      ++count.complete;
      if (hi >= 0 && lo >= 0 && sum == ((hi << 4) | lo)) ++count.valid;
      i = j + 3;
    }
    return count;
  }

} // namespace


int main() {
  stdio_init_all();
  sleep_ms(Test::STARTUP_DELAY_MS);

  printf("\n=== GNSS driver bench test (Orion B16-C) ===\n");
  printf("UART1 RX GP%d (TX GP%d reserved, not driven), %lu baud 8N1\n", Cfg::UART_RX, Cfg::UART_TX, static_cast<unsigned long>(Cfg::BAUD));
  printf("Chunk %u bytes, read timeout %lu ms, period %lu ms\n", static_cast<unsigned>(Cfg::READ_LENGTH), static_cast<unsigned long>(Cfg::READ_TIMEOUT_MS),
         static_cast<unsigned long>(Test::PERIOD_MS));

  if (!GNSS::Init()) {
    printf("[FAIL] GNSS::Init(): UART_RX is unassigned in gnss_config.h. Halting.\n");
    for (;;)
      tight_loop_contents();
  }
  printf("[INFO] GNSS::Init() done\n");

  unsigned reads = 0, ok = 0, failed = 0;
  GNSS::Chunk chunk {};
  for (;;) {
    ++reads;
    const absolute_time_t start = get_absolute_time();
    const GNSS::Result result = GNSS::Read(chunk);
    const int64_t took_ms = absolute_time_diff_us(start, get_absolute_time()) / 1000;

    printf("\n--- read %u: %s (%lld ms) ---\n", reads, GNSS::ResultName(result), static_cast<long long>(took_ms));
    if (result == GNSS::Result::Ok) {
      ++ok;
      HexDump(chunk);
      const SentenceCount sentences = CheckSentences(chunk);
      printf("[%s] %u of %u complete sentences have a matching checksum\n", sentences.complete > 0 && sentences.valid == sentences.complete ? "PASS" : "FAIL",
             sentences.valid, sentences.complete);
    } else {
      ++failed;
    }
    printf("tally: %u reads, %u Ok, %u failed\n", reads, ok, failed);

    sleep_ms(Test::PERIOD_MS);
  }
}
