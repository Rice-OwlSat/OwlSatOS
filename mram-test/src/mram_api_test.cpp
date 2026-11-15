/**

    @file      mram_api_test.cpp
    @brief     Bench test for the MRAM driver API (mram.h) on the AS3016204 test board.
    @details   Prints its results over UART0 (GP0 TX / GP1 RX, 115200 8N1) to the debug probe.
               Built three times:
               - MRAM_Test_Read  (no_flash, MRAM_TEST_WRITES=0): only sends read instructions.
                 Changes nothing.
               - MRAM_Test_Write (no_flash, MRAM_TEST_WRITES=1): the read tests, then a same-value
                 register write and an array write in the upper 1 MiB, which is restored after.
               - MRAM_Test_XIP   (executes from the MRAM, MRAM_TEST_WRITES=1, MRAM_TEST_XIP=1): the
                 write suite from a FreeRTOS task, through the XIP session transport, while a second
                 task keeps the scheduler switching. Adds reads of the written data through the XIP
                 window, which check the cache flush. This one replaces whatever image is in the
                 bottom of the MRAM.

               Every step prints PASS, FAIL or INFO, then a summary line that repeats every
               Config::MRAMTest::SUMMARY_PERIOD_MS. GP25 lights when every check passed, GP24 when
               any failed.

               Never sends anything that changes the chip's interface mode. Only the write area in
               the upper half of the array is ever written, and it is restored.

    @author    Viola Case
    @date      15.11.2026
    @copyright © Viola Case, 2026. All right reserved.

**/

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>
#include <type_traits>

#include <hardware/clocks.h>
#include <pico/stdlib.h>

#include <OwlSat/mram.h>

#ifndef MRAM_TEST_WRITES
#error "Build through an MRAM_Test_* target, which define MRAM_TEST_WRITES"
#endif
#ifndef MRAM_TEST_XIP
#define MRAM_TEST_XIP 0
#endif

#if MRAM_TEST_XIP
#include <hardware/regs/addressmap.h>

#include "FreeRTOS.h"
#include "task.h"
#endif


namespace {

  using namespace OwlSat::MRAM;
  namespace Test = OwlSat::Config::MRAMTest;
  using Part = AS3016204;
  using Settings = OwlSat::Config::MRAMSettings<Part>;

  static_assert(std::is_same_v<OwlSat::Config::MRAMPart, Part>, "The bench tests are written for the AS3016204 test board");
  static_assert(MRAM_TEST_XIP == OwlSat::Config::MRAM_USES_XIP_SESSIONS, "MRAM_Test_XIP must use XIP sessions, the others must not");
  static_assert(Test::WRITE_ADDRESS >= Part::CAPACITY / 2 && Test::WRITE_ADDRESS + Test::WRITE_LENGTH <= Part::CAPACITY,
                "The write test must stay inside the unprotected upper half of the array");

  // Test-board LEDs. Not taken from OwlSat::Pins: on the flight harness GPIO 24 is BURN_0.
  constexpr uint LED_PASS = 25; //!< Solid: every check passed
  constexpr uint LED_FAIL = 24; //!< Solid: at least one check failed

  unsigned g_passed = 0; //!< Checks passed so far
  unsigned g_failed = 0; //!< Checks failed so far

  /*
   * ===========================================
   * Helpers
   * ===========================================
   */

  /*!
   * @brief   Records and prints one check.
   * @param   ok    Whether it passed
   * @param   what  What was checked
   * @return  ok
   */
  bool Check(bool ok, const char *what) {
    (ok ? g_passed : g_failed)++;
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    return ok;
  }

  /*!
   * @brief   Prints bytes as hex, 16 per line, each line prefixed with its address.
   * @param   address  Address of the first byte
   * @param   bytes    The bytes
   */
  void HexDump(uint32_t address, std::span<const uint8_t> bytes) {
    for (size_t i = 0; i < bytes.size(); ++i) {
      if (i % 16 == 0) printf("%s  %06X:", i ? "\n" : "", static_cast<unsigned>(address + i));
      printf(" %02X", bytes[i]);
    }
    printf("\n");
  }

  /*!
   * @brief   Runs a no-address read instruction straight through Execute(), bypassing the driver API.
   * @param   instr  The instruction
   * @param   rx     Receives the data
   * @return  False if Execute() refused
   */
  bool Raw(const Instruction &instr, std::span<uint8_t> rx) { return Execute(instr, 0, {}, rx); }

  /*!
   * @brief   Prints a register table against the chip, marking which registers differ.
   * @param   title     Heading
   * @param   expected  The table to compare with
   */
  void PrintDiff(const char *title, std::span<const RegisterDefault> expected) {
    printf("%s:\n", title);
    for (const RegisterDefault &reg : expected) {
      std::optional<uint8_t> actual = ReadRegister(reg.address);
      if (!actual) {
        printf("  %02Xh  expected %02X  read FAILED\n", static_cast<unsigned>(reg.address), reg.value);
        continue;
      }
      bool same = ((*actual ^ reg.value) & reg.mask) == 0;
      printf("  %02Xh  expected %02X  actual %02X  mask %02X  %s\n", static_cast<unsigned>(reg.address), reg.value, *actual, reg.mask,
             same ? "same" : "DIFFERS");
    }
  }

  /*
   * ===========================================
   * Read tests
   * ===========================================
   */

  /// 1. RDID through the API
  void TestID() {
    printf("\n--- 1. Device ID ---\n");
    std::array<uint8_t, 4> id {};
    bool ok = ReadID(id);
    printf("RDID:");
    for (uint8_t b : id) printf(" %02X", b);
    printf("   expected:");
    for (uint8_t b : Part::EXPECTED_ID) printf(" %02X", b);
    printf("\n");
    Check(ok && id == Part::EXPECTED_ID, "ReadID() returns the AS3016204's ID");
    Check(ok && IsExpectedID(id), "IsExpectedID() accepts it");
  }

  /// 2. RDAR reads against the part's dedicated register-read opcodes
  void TestRegisterReads() {
    printf("\n--- 2. ReadRegister() (RDAR) vs dedicated read opcodes ---\n");
    std::array<uint8_t, 1> sr {};
    std::array<uint8_t, 4> crx {};
    std::array<std::array<uint8_t, 1>, 4> cr {};
    bool raw_ok = Raw(Instruction::ReadStatusRegister, sr) && Raw(Part::ReadConfigurationRegisters, crx) &&
                  Raw(Part::ReadConfigurationRegister1, cr[0]) && Raw(Part::ReadConfigurationRegister2, cr[1]) &&
                  Raw(Part::ReadConfigurationRegister3, cr[2]) && Raw(Part::ReadConfigurationRegister4, cr[3]);
    Check(raw_ok, "Raw RDSR / RDCX / RDC1..RDC4 accepted by Execute()");

    std::optional<uint8_t> sr_rdar = ReadRegister(Part::SR::ADDRESS);
    printf("SR   RDSR %02X   RDAR %s%02X\n", sr[0], sr_rdar ? "" : "FAILED ", sr_rdar.value_or(0));
    Check(sr_rdar && *sr_rdar == sr[0], "ReadRegister(SR) == RDSR");

    constexpr std::array<uint32_t, 4> CR_ADDRESSES { Part::CR1::ADDRESS, Part::CR2::ADDRESS, Part::CR3::ADDRESS, Part::CR4::ADDRESS };
    bool all = true;
    for (size_t i = 0; i < CR_ADDRESSES.size(); ++i) {
      std::optional<uint8_t> rdar = ReadRegister(CR_ADDRESSES[i]);
      bool same = rdar && *rdar == cr[i][0] && *rdar == crx[i];
      all &= same;
      printf("CR%u  RDC%u %02X   RDCX %02X   RDAR %s%02X   %s\n", static_cast<unsigned>(i + 1), static_cast<unsigned>(i + 1), cr[i][0], crx[i],
             rdar ? "" : "FAILED ", rdar.value_or(0), same ? "same" : "DIFFERS");
    }
    Check(all, "ReadRegister(CR1..CR4) == RDCn == RDCX");
  }

  /// 3. Init() and a diff against the flight and factory configurations
  void TestInit() {
    printf("\n--- 3. Init() ---\n");
    Result result = Init();
    printf("Init() = %s, MLATS now %u\n", ResultName(result), static_cast<unsigned>(detail::MemoryLatency()));
    Check(result == Result::Ok || result == Result::ConfigMismatch, "Init() brings the driver up (Ok, or ConfigMismatch which is non-fatal)");
    if (result == Result::ConfigMismatch) printf("[INFO] ConfigMismatch: the chip isn't in the flight configuration. Not a failure.\n");
    printf("[INFO] VerifyConfig() = %s\n", VerifyConfig() ? "true" : "false");
    PrintDiff("Flight configuration (FLIGHT_DEFAULTS)", Settings::FLIGHT_DEFAULTS);
    PrintDiff("Factory configuration (FACTORY_DEFAULTS)", Settings::FACTORY_DEFAULTS);
  }

  /// 4. Chunked array reads: overlapping reads at two offsets, plus single-byte reads at the chunk boundary
  void TestArrayRead() {
    printf("\n--- 4. Read() ---\n");
    std::array<uint8_t, Test::READ_LENGTH> a {};
    std::array<uint8_t, Test::READ_LENGTH - 1> b {};
    Result ra = Read(Test::READ_ADDRESS, a);
    Result rb = Read(Test::READ_ADDRESS + 1, b);
    printf("Read(%06X, %u) = %s, Read(%06X, %u) = %s\n", static_cast<unsigned>(Test::READ_ADDRESS), static_cast<unsigned>(a.size()), ResultName(ra),
           static_cast<unsigned>(Test::READ_ADDRESS + 1), static_cast<unsigned>(b.size()), ResultName(rb));
    HexDump(Test::READ_ADDRESS, a);
    Check(ra == Result::Ok && rb == Result::Ok, "Read() succeeds across a chunk boundary");

    bool overlap = std::equal(b.begin(), b.end(), a.begin() + 1);
    Check(overlap, "Read() at +1 matches Read() at +0, shifted by one byte");

    bool blank = std::all_of(a.begin(), a.end(), [&](uint8_t v) { return v == a[0]; });
    if (blank) printf("[INFO] Every byte read is %02X. Expected blinky's image here; is it still programmed?\n", a[0]);

    const size_t chunk = Instruction::ReadMemoryArray.MaxDataBytes();
    printf("[INFO] Read chunk size %u bytes\n", static_cast<unsigned>(chunk));
    bool singles = true;
    for (size_t i : { chunk - 1, chunk, chunk + 1 }) {
      if (i >= a.size()) continue;
      std::array<uint8_t, 1> one {};
      bool ok = Execute(Instruction::ReadMemoryArray, Test::READ_ADDRESS + i, {}, one);
      printf("  byte %06X  single read %02X  Read() %02X\n", static_cast<unsigned>(Test::READ_ADDRESS + i), one[0], a[i]);
      singles &= ok && one[0] == a[i];
    }
    Check(singles, "Single-byte reads at the chunk boundary match Read()");
  }

  /// 5. Range checks. Nothing is sent to the chip
  void TestRange() {
    printf("\n--- 5. Range checks ---\n");
    std::array<uint8_t, 2> two {};
    std::array<uint8_t, 1> one {};
    Check(Read(Part::CAPACITY - 1, two) == Result::OutOfRange, "Read() of 2 bytes at CAPACITY-1 is OutOfRange");
    Check(Read(Part::CAPACITY - 1, one) == Result::Ok, "Read() of the last byte is allowed");
    Check(Read(Part::CAPACITY, one) == Result::OutOfRange, "Read() at CAPACITY is OutOfRange");
  }

#if MRAM_TEST_WRITES
  /*
   * ===========================================
   * Write tests
   * ===========================================
   */

  /// 6. Write CR3 with the value it already holds
  void TestRegisterWrite() {
    printf("\n--- 6. WriteRegister(CR3, current value) ---\n");
    std::optional<uint8_t> before = ReadRegister(Part::CR3::ADDRESS);
    if (!Check(before.has_value(), "Read CR3 before writing")) return;
    Result result = WriteRegister(Part::CR3::ADDRESS, *before, Part::CR3::WRITABLE);
    std::optional<uint8_t> after = ReadRegister(Part::CR3::ADDRESS);
    printf("CR3 before %02X, WriteRegister() = %s, after %02X\n", *before, ResultName(result), after.value_or(0));
    Check(result == Result::Ok, "WriteRegister() verifies its own read-back");
    Check(after && *after == *before, "CR3 is unchanged");
  }

  /// 7. Write a pattern in the upper half, read it back, then restore the original contents
  void TestArrayWrite() {
    printf("\n--- 7. Write() at %06X, %u bytes ---\n", static_cast<unsigned>(Test::WRITE_ADDRESS), static_cast<unsigned>(Test::WRITE_LENGTH));
    std::array<uint8_t, Test::WRITE_LENGTH> original {};
    std::array<uint8_t, Test::WRITE_LENGTH> pattern {};
    std::array<uint8_t, Test::WRITE_LENGTH> readback {};

    if (!Check(Read(Test::WRITE_ADDRESS, original) == Result::Ok, "Save the original contents")) return;
    printf("Original contents:\n");
    HexDump(Test::WRITE_ADDRESS, original);

    // Inverted, so every byte differs from the original in every bit
    for (size_t i = 0; i < pattern.size(); ++i) pattern[i] = static_cast<uint8_t>(~original[i]);
    Check(Write(Test::WRITE_ADDRESS, pattern) == Result::Ok, "Write() the pattern (original bytes inverted)");
    Check(Read(Test::WRITE_ADDRESS, readback) == Result::Ok && readback == pattern, "Read() returns the pattern");
#if MRAM_TEST_XIP
    // The session flushed the XIP cache, so the cached window must already show the new data
    const volatile uint8_t *cached = reinterpret_cast<const volatile uint8_t *>(XIP_BASE + Test::WRITE_ADDRESS);
    const volatile uint8_t *uncached = reinterpret_cast<const volatile uint8_t *>(XIP_NOCACHE_NOALLOC_BASE + Test::WRITE_ADDRESS);
    bool cached_ok = true, uncached_ok = true;
    for (size_t i = 0; i < pattern.size(); ++i) {
      cached_ok &= cached[i] == pattern[i];
      uncached_ok &= uncached[i] == pattern[i];
    }
    Check(cached_ok, "The cached XIP window shows the pattern (the session flushed the cache)");
    Check(uncached_ok, "The uncached XIP window shows the pattern");
#endif

    Result restore = Write(Test::WRITE_ADDRESS, original);
    bool restored = restore == Result::Ok && Read(Test::WRITE_ADDRESS, readback) == Result::Ok && readback == original;
    Check(restored, "Write() the original back, and Read() confirms it");
    if (!restored) {
      printf("!!! RESTORE FAILED. Contents now:\n");
      HexDump(Test::WRITE_ADDRESS, readback);
      printf("!!! They should be the 'Original contents' dump above.\n");
    }
  }
#endif

  /// Prints the summary line and sets the LEDs
  void Summary() {
    printf("=== SUMMARY %u/%u PASS%s ===\n", g_passed, g_passed + g_failed, g_failed ? " -- FAILURES" : "");
    gpio_put(LED_PASS, g_failed == 0);
    gpio_put(LED_FAIL, g_failed != 0);
  }

  /// Prints the banner and runs every test this build includes
  void RunTests() {
    const char *variant = MRAM_TEST_XIP ? "MRAM_Test_XIP, from a FreeRTOS task" : MRAM_TEST_WRITES ? "MRAM_Test_Write" : "MRAM_Test_Read, read-only";
    uint32_t sys_hz = clock_get_hz(clk_sys);
    printf("\n=== MRAM API bench test (%s) ===\n", variant);
    printf("Part %s, %u KiB. Transport: %s. Direct-mode SCK = %u kHz / %u = %u kHz\n", Part::NAME, static_cast<unsigned>(Part::CAPACITY / 1024),
           OwlSat::Config::MRAM_USES_XIP_SESSIONS ? "XIP sessions" : "direct", static_cast<unsigned>(sys_hz / 1000),
           static_cast<unsigned>(OwlSat::Config::MRAM_DIRECT_CLKDIV), static_cast<unsigned>(sys_hz / OwlSat::Config::MRAM_DIRECT_CLKDIV / 1000));

    TestID();
    TestRegisterReads();
    TestInit();
    TestArrayRead();
    TestRange();
#if MRAM_TEST_WRITES
    TestRegisterWrite();
    TestArrayWrite();
#endif
  }

  /// Sets up the LEDs and stdio, then waits for a serial reader to attach
  void StartBoard() {
    for (uint pin : { LED_PASS, LED_FAIL }) {
      gpio_init(pin);
      gpio_set_dir(pin, GPIO_OUT);
    }
    stdio_init_all();
    sleep_ms(Test::STARTUP_DELAY_MS);
  }

#if MRAM_TEST_XIP
  /*
   * ===========================================
   * FreeRTOS harness
   * ===========================================
   */

  volatile uint32_t g_busy_wakeups = 0; //!< Times the busy task has run

  /*!
   * @brief   Wakes every tick, so the scheduler keeps switching tasks and the tick interrupt keeps
   *          firing while the tests take the MRAM away from XIP.
   * @param   params  Unused
   */
  void BusyTask(void *params) {
    (void)params;
    for (;;) {
      g_busy_wakeups = g_busy_wakeups + 1;
      vTaskDelay(1);
    }
  }

  /*!
   * @brief   Runs the tests, checks the scheduler kept going, then repeats the summary.
   * @param   params  Unused
   */
  void TestTask(void *params) {
    (void)params;
    TickType_t ticks_before = xTaskGetTickCount();
    uint32_t wakeups_before = g_busy_wakeups;
    RunTests();
    TickType_t ticks = xTaskGetTickCount() - ticks_before;
    uint32_t wakeups = g_busy_wakeups - wakeups_before;

    printf("\n--- 8. Scheduler ---\n");
    printf("Ticks during the tests: %u, busy task wake-ups: %u\n", static_cast<unsigned>(ticks), static_cast<unsigned>(wakeups));
    Check(ticks > 0 && wakeups > 0, "The tick and the other task kept running around the XIP sessions");

    printf("\n");
    for (;;) {
      Summary();
      vTaskDelay(pdMS_TO_TICKS(Test::SUMMARY_PERIOD_MS));
    }
  }
#endif

} // namespace


#if MRAM_TEST_XIP
extern "C" void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
  (void)task;
  printf("!!! stack overflow in task %s\n", name);
  for (;;) {}
}

extern "C" void vApplicationMallocFailedHook() {
  printf("!!! FreeRTOS heap exhausted\n");
  for (;;) {}
}

int main() {
  StartBoard();
  xTaskCreate(TestTask, "mram_test", Test::TEST_TASK_STACK_WORDS, nullptr, 1, nullptr);
  xTaskCreate(BusyTask, "busy", configMINIMAL_STACK_SIZE, nullptr, 1, nullptr);
  vTaskStartScheduler();
  for (;;) {}
}
#else
int main() {
  StartBoard();
  RunTests();
  printf("\n");
  for (;;) {
    Summary();
    sleep_ms(Test::SUMMARY_PERIOD_MS);
  }
}
#endif
