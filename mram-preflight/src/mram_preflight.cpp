/**

    @file      mram_preflight.cpp
    @brief     Bench probe: puts the MRAM into pre-flight storage mode, and checks it is there.
    @details   SRAM-only (no_flash) like the MRAM_Test_* binaries, with stdio on UART0 (GP0 TX / GP1 RX,
               115200 8N1) for the debug probe. Load it with `picotool load -x`; it never touches the
               image at the bottom of the MRAM.

               Pre-flight storage mode is defined in Config::MRAMPreflight (mram_config.h) and is
               provisional. The probe is a list of steps, each of which first checks whether the chip
               is already in that step's state and only writes if it is not, so running it twice is
               harmless and a second run (e.g. after a power cycle) is the persistence check: it
               should report every step as "already set" and write nothing.

               Steps today:
               1. Registers. Every entry of Config::MRAMPreflight::REGISTERS is written with
                  WriteRegister() and read back under its mask. Today that is the flight set: CR1.MAPLK
                  (locks SR block protection as it stands), CR2 MLATS 0, CR3 factory drive, and
                  CR4.WRENS = Normal (every array write needs its own WREN). SR is reported but not
                  written.
               Then a check that changes nothing: an array write and read-back in the scratch area,
               restored afterwards, to show the chip still takes writes the way the driver sends them
               in this mode.

               Adding to the mode means adding a Step to STEPS below, and its values to mram_config.h.

               GP25 lights when every step and check passed, GP24 when any failed.

    @author    Viola Case
    @date      15.11.2026
    @copyright © Viola Case, 2026. All right reserved.

**/

#include <array>
#include <cstdio>
#include <span>
#include <type_traits>

#include <pico/stdlib.h>

#include <OwlSat/mram.h>


namespace {

  using namespace OwlSat::MRAM;
  namespace Preflight = OwlSat::Config::MRAMPreflight;
  namespace Test = OwlSat::Config::MRAMTest;
  using Part = OwlSat::Config::MRAMPart;

  static_assert(std::is_same_v<Part, AS3016204>, "Only benched on the AS3016204 test board");
  static_assert(!OwlSat::Config::MRAM_USES_XIP_SESSIONS, "MRAM_Preflight runs from SRAM and drives the QMI directly");
  static_assert(Preflight::SCRATCH_ADDRESS >= Part::CAPACITY / 2 && Preflight::SCRATCH_ADDRESS + Preflight::SCRATCH_LENGTH <= Part::CAPACITY,
                "The scratch area must stay inside the unprotected upper half of the array");

  // Test-board LEDs. Not taken from OwlSat::Pins: on the flight harness GPIO 24 is BURN_0.
  constexpr uint LED_PASS = 25; //!< Solid: every step and check passed
  constexpr uint LED_FAIL = 24; //!< Solid: at least one failed

  unsigned g_passed = 0; //!< Checks passed so far
  unsigned g_failed = 0; //!< Checks failed so far

  bool Check(bool ok, const char *what) {
    (ok ? g_passed : g_failed)++;
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    return ok;
  }

  /// Name of a register address on this part, for the log
  const char *RegisterName(uint32_t address) {
    switch (address) {
    case Part::SR::ADDRESS: return "SR";
    case Part::CR1::ADDRESS: return "CR1";
    case Part::CR2::ADDRESS: return "CR2";
    case Part::CR3::ADDRESS: return "CR3";
    case Part::CR4::ADDRESS: return "CR4";
    }
    return "?";
  }

  /// SR, decoded. Reported because MAPLK freezes its block protection where it stands
  void PrintStatusRegister() {
    std::optional<uint8_t> sr = ReadRegister(Part::SR::ADDRESS);
    if (!sr) {
      printf("SR   read FAILED\n");
      return;
    }
    unsigned bpsel = (*sr & Part::SR::BPSEL_MASK) >> Part::SR::BPSEL_SHIFT;
    printf("SR   %02X  WPEN %u  TBSEL %u (%s)  BPSEL %u%s\n", *sr, (*sr & Part::SR::WPEN) ? 1u : 0u, (*sr & Part::SR::TBSEL) ? 1u : 0u,
           (*sr & Part::SR::TBSEL) ? "bottom" : "top", bpsel, bpsel == 0 ? " (nothing protected)" : "");
  }

  /*
   * ===========================================
   * Steps
   * ===========================================
   */

  /// One part of pre-flight storage mode
  struct Step {
    const char *name;
    bool (*is_set)(); //!< Reads only. Whether the chip is already in this step's state
    bool (*apply)();  //!< Writes, then verifies. Only called when is_set() is false
  };

  /// Whether one register matches its entry under the entry's mask
  bool RegisterMatches(const RegisterDefault &reg, bool print) {
    std::optional<uint8_t> value = ReadRegister(reg.address);
    bool same = value && ((*value ^ reg.value) & reg.mask) == 0;
    if (print) {
      printf("  %-3s %02Xh  want %02X  have %s%02X  mask %02X  %s\n", RegisterName(reg.address), static_cast<unsigned>(reg.address), reg.value,
             value ? "" : "FAILED ", value.value_or(0), reg.mask, same ? "same" : "DIFFERS");
    }
    return same;
  }

  bool RegistersSet() {
    bool all = true;
    for (const RegisterDefault &reg : Preflight::REGISTERS) all &= RegisterMatches(reg, true);
    return all;
  }

  bool ApplyRegisters() {
    bool all = true;
    for (const RegisterDefault &reg : Preflight::REGISTERS) {
      if (RegisterMatches(reg, false)) continue;
      Result result = WriteRegister(reg.address, reg.value, reg.mask);
      printf("  WriteRegister(%s, %02X) = %s\n", RegisterName(reg.address), reg.value, ResultName(result));
      all &= result == Result::Ok;
    }
    return all;
  }

  /// Pre-flight storage mode, in the order the steps are applied. Add new steps here
  constexpr std::array STEPS {
    Step { "Registers (Config::MRAMPreflight::REGISTERS)", RegistersSet, ApplyRegisters },
  };

  /*
   * ===========================================
   * Checks that change nothing
   * ===========================================
   */

  /// Array write and read-back in the scratch area, then restore it
  void CheckArrayWrite() {
    printf("\n--- Array write in this mode, at %06X, %u bytes (restored) ---\n", static_cast<unsigned>(Preflight::SCRATCH_ADDRESS),
           static_cast<unsigned>(Preflight::SCRATCH_LENGTH));
    std::array<uint8_t, Preflight::SCRATCH_LENGTH> original {};
    std::array<uint8_t, Preflight::SCRATCH_LENGTH> pattern {};
    std::array<uint8_t, Preflight::SCRATCH_LENGTH> readback {};

    if (!Check(Read(Preflight::SCRATCH_ADDRESS, original) == Result::Ok, "Save the scratch contents")) return;
    for (size_t i = 0; i < pattern.size(); ++i) pattern[i] = static_cast<uint8_t>(~original[i]);

    Check(Write(Preflight::SCRATCH_ADDRESS, pattern) == Result::Ok && Read(Preflight::SCRATCH_ADDRESS, readback) == Result::Ok && readback == pattern,
          "Write() lands and reads back (WREN per chunk)");

    bool restored = Write(Preflight::SCRATCH_ADDRESS, original) == Result::Ok && Read(Preflight::SCRATCH_ADDRESS, readback) == Result::Ok &&
                    readback == original;
    Check(restored, "Scratch area restored");
    if (!restored) printf("!!! RESTORE FAILED at %06X: the scratch area is not what it was\n", static_cast<unsigned>(Preflight::SCRATCH_ADDRESS));
  }

  /*
   * ===========================================
   * Run
   * ===========================================
   */

  void Run() {
    printf("\n=== MRAM pre-flight storage mode (%s, %u KiB) ===\n", Part::NAME, static_cast<unsigned>(Part::CAPACITY / 1024));

    Result init = Init();
    printf("Init() = %s\n", ResultName(init));
    if (!Check(init == Result::Ok || init == Result::ConfigMismatch, "Init() brings the driver up")) return;

    PrintStatusRegister();

    unsigned written = 0;
    for (size_t i = 0; i < STEPS.size(); ++i) {
      const Step &step = STEPS[i];
      printf("\n--- Step %u: %s ---\n", static_cast<unsigned>(i + 1), step.name);
      if (step.is_set()) {
        Check(true, "Already set; nothing written");
        continue;
      }
      ++written;
      bool applied = step.apply();
      printf("  after:\n");
      Check(applied && step.is_set(), "Applied and verified");
    }

    CheckArrayWrite();

    printf("\n");
    PrintStatusRegister();
    printf("[INFO] %u of %u steps needed writing. Power-cycle and run again: every step should say \"Already set\".\n", written,
           static_cast<unsigned>(STEPS.size()));
  }

  void Summary() {
    printf("=== SUMMARY %u/%u PASS%s ===\n", g_passed, g_passed + g_failed, g_failed ? " -- FAILURES" : "");
    gpio_put(LED_PASS, g_failed == 0);
    gpio_put(LED_FAIL, g_failed != 0);
  }

} // namespace


int main() {
  for (uint pin : { LED_PASS, LED_FAIL }) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
  }
  stdio_init_all();
  sleep_ms(Test::STARTUP_DELAY_MS);

  Run();
  printf("\n");
  for (;;) {
    Summary();
    sleep_ms(Test::SUMMARY_PERIOD_MS);
  }
}
