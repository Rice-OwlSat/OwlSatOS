/**

    @file      antenna_deploy_task.cpp
    @brief     Antenna deployment: fire the burn wire once, then latch that it happened.
    @details   The latch is several copies of one 32-bit word in the MRAM, read by majority vote with
               a per-copy bit-error tolerance. The encoding and its reasoning are in the antenna
               section of config.h.

               This task is, for now, the only user of the MRAM in the flight image, so it brings
               the driver up itself and takes no lock. Once a second task talks to the MRAM, the
               driver needs the mutex the README lists as open, and MRAM::Init() should move to
               boot.
    @author    Viola Case
    @date      15.11.2026
    @copyright © Viola Case, 2026. All right reserved.

**/

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>

#include "FreeRTOS.h"
#include "task.h"
#include <hardware/gpio.h>

#include <OwlSat/config.h>
#include <OwlSat/mram.h>
#include <OwlSat/pin_assignment.h>
#include <OwlSat/tasks.h>

namespace OwlSat {

  namespace {

    constexpr auto &LATCH_ADDRESSES = Config::ANTENNA_LATCH_ADDRESSES;
    constexpr size_t LATCH_COPIES = LATCH_ADDRESSES.size();
    constexpr size_t LATCH_MAJORITY = LATCH_COPIES / 2 + 1;
    constexpr size_t LATCH_BYTES = sizeof(Config::ANTENNA_LATCH_DEPLOYED);

    static_assert(LATCH_COPIES >= 3 && LATCH_COPIES % 2 == 1, "The latch needs an odd number of copies, at least three, to vote");
    static_assert(2 * Config::ANTENNA_LATCH_MAX_BIT_ERRORS < unsigned(std::popcount(Config::ANTENNA_LATCH_DEPLOYED))
                      && 2 * Config::ANTENNA_LATCH_MAX_BIT_ERRORS < unsigned(std::popcount(~Config::ANTENNA_LATCH_DEPLOYED)),
                  "Bit-error tolerance too loose: all-zero or all-one MRAM could be read as deployed");
    static_assert(Pins::IsAssigned(Pins::BURN_0), "BURN_0 has no pin");

    /// Every copy inside the array and no two overlapping
    consteval bool LatchAddressesValid() {
      for (size_t i = 0; i < LATCH_COPIES; ++i) {
        if (LATCH_ADDRESSES[i] > Config::MRAMPart::CAPACITY - LATCH_BYTES) return false;
        for (size_t j = i + 1; j < LATCH_COPIES; ++j) {
          const uint32_t lo = std::min(LATCH_ADDRESSES[i], LATCH_ADDRESSES[j]);
          const uint32_t hi = std::max(LATCH_ADDRESSES[i], LATCH_ADDRESSES[j]);
          if (hi - lo < LATCH_BYTES) return false;
        }
      }
      return true;
    }
    static_assert(LatchAddressesValid(), "A latch copy is outside the MRAM or overlaps another copy");

    /// What one copy of the latch says
    enum class CopyState : uint8_t {
      Exact,      //!< Holds the deployed word, bit for bit
      Corrected,  //!< Within the bit-error tolerance of the deployed word. Counts as deployed
      Clear,      //!< Anything else
      Unreadable, //!< The MRAM refused the read
    };

    constexpr bool CountsAsDeployed(CopyState state) { return state == CopyState::Exact || state == CopyState::Corrected; }

    const char *CopyStateName(CopyState state) {
      switch (state) {
      case CopyState::Exact: return "set";
      case CopyState::Corrected: return "set (bit errors)";
      case CopyState::Clear: return "clear";
      case CopyState::Unreadable: return "unreadable";
      }
      return "?";
    }

    struct LatchReading {
      std::array<CopyState, LATCH_COPIES> copies {};
      std::array<uint32_t, LATCH_COPIES> words {}; //!< Raw contents of each copy, for the log
      size_t votes = 0; //!< Copies that count as deployed

      bool Deployed() const { return votes >= LATCH_MAJORITY; }

      bool AllExact() const {
        for (CopyState c : copies) {
          if (c != CopyState::Exact) return false;
        }
        return true;
      }
    };

    CopyState ReadCopy(uint32_t address, uint32_t *word_out = nullptr) {
      std::array<uint8_t, LATCH_BYTES> raw {};
      if (MRAM::Read(address, raw) != MRAM::Result::Ok) return CopyState::Unreadable;

      const uint32_t word = std::bit_cast<uint32_t>(raw);
      if (word_out != nullptr) *word_out = word;
      const unsigned errors = std::popcount(word ^ Config::ANTENNA_LATCH_DEPLOYED);
      if (errors == 0) return CopyState::Exact;
      if (errors <= Config::ANTENNA_LATCH_MAX_BIT_ERRORS) return CopyState::Corrected;
      return CopyState::Clear;
    }

    LatchReading ReadLatch() {
      LatchReading reading;
      for (size_t i = 0; i < LATCH_COPIES; ++i) {
        reading.copies[i] = ReadCopy(LATCH_ADDRESSES[i], &reading.words[i]);
        if (CountsAsDeployed(reading.copies[i])) ++reading.votes;
      }
      return reading;
    }

    void PrintLatch(const LatchReading &reading) {
      for (size_t i = 0; i < LATCH_COPIES; ++i) {
        printf("[antenna]   copy %u @ 0x%06lx: %08lx  %s\n", unsigned(i), (unsigned long) LATCH_ADDRESSES[i],
               (unsigned long) reading.words[i], CopyStateName(reading.copies[i]));
      }
    }

    /// Writes the deployed word to one copy and reads it back. MRAM::Write() does not verify
    bool WriteCopy(uint32_t address) {
      const auto raw = std::bit_cast<std::array<uint8_t, LATCH_BYTES>>(Config::ANTENNA_LATCH_DEPLOYED);
      if (MRAM::Write(address, raw) != MRAM::Result::Ok) return false;
      return ReadCopy(address) == CopyState::Exact;
    }

    /// Rewrites every copy that is not exact. Used both to set the latch and to scrub it
    void WriteLatch(const LatchReading &before) {
      for (size_t i = 0; i < LATCH_COPIES; ++i) {
        if (before.copies[i] == CopyState::Exact) continue;
        if (!WriteCopy(LATCH_ADDRESSES[i])) {
          printf("[antenna] WARNING: latch copy %u @ 0x%06lx did not verify after writing\n", unsigned(i),
                 (unsigned long) LATCH_ADDRESSES[i]);
        }
      }
    }

    /// BURN_0 high for Config::ANTENNA_BURN_MS, then low
    void FireBurnWire() {
      // Low before output, so enabling the driver cannot glitch the switch on early
      gpio_init(Pins::BURN_0);
      gpio_put(Pins::BURN_0, false);
      gpio_set_dir(Pins::BURN_0, GPIO_OUT);

      printf("[antenna] BURN_0 (GPIO %d) on for %lu ms\n", Pins::BURN_0, (unsigned long) Config::ANTENNA_BURN_MS);
      gpio_put(Pins::BURN_0, true);
      vTaskDelay(pdMS_TO_TICKS(Config::ANTENNA_BURN_MS));
      gpio_put(Pins::BURN_0, false);
      printf("[antenna] BURN_0 off\n");
    }

    /// Nothing left to do: lowest priority, and suspended so it never competes with the idle task
    [[noreturn]] void Retire() {
      vTaskPrioritySet(nullptr, tskIDLE_PRIORITY);
      for (;;) vTaskSuspend(nullptr);
    }

  } // namespace

  void AntennaDeployTask(void *params) {
    (void) params;

    // ConfigMismatch means the chip works but its registers are not the flight set, which says
    // nothing about the array. Anything else means the latch can be neither read nor written.
    const MRAM::Result init = MRAM::Init();
    const bool mram_ok = init == MRAM::Result::Ok || init == MRAM::Result::ConfigMismatch;

    LatchReading latch;
    if (mram_ok) {
      latch = ReadLatch();
      printf("[antenna] deployed latch: %u of %u copies set\n", unsigned(latch.votes), unsigned(LATCH_COPIES));
      PrintLatch(latch);

      if (latch.Deployed()) {
        // Repair any copy that has picked up bit errors or been lost, so the next fault has the
        // full margin to work against rather than whatever this one left
        if (!latch.AllExact()) {
          printf("[antenna] scrubbing latch\n");
          WriteLatch(latch);
        }
        printf("[antenna] antennae already deployed; task retiring at priority 0\n");
        Retire();
      }
    } else {
      printf("[antenna] WARNING: MRAM init failed (%s). Deploying anyway; the latch cannot be set, so this "
             "will repeat every boot\n",
             MRAM::ResultName(init));
    }

    FireBurnWire();

    // =============================================================================================
    // TODO !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    // TODO !!!  WAIT FOR DEPLOYMENT CONFIRMATION HERE BEFORE SETTING THE LATCH.               !!!
    // TODO !!!                                                                                 !!!
    // TODO !!!  Confirmation (deployment switch, current sense on the burn rail, radio check —  !!!
    // TODO !!!  whatever the hardware gives us) is meant to be a SEPARATE TASK. This task should !!!
    // TODO !!!  block on it here, and fire again or report failure if it never comes.            !!!
    // TODO !!!                                                                                 !!!
    // TODO !!!  UNTIL THEN THE LATCH IS SET UNCONDITIONALLY AFTER ONE BURN. A burn that does    !!!
    // TODO !!!  not cut the wire is NEVER RETRIED, on this boot or any other.                  !!!
    // TODO !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    // =============================================================================================
#warning "antenna_deploy_task.cpp: NO DEPLOYMENT CONFIRMATION. The deployed latch is set after one burn whether or not the antennae came out. See the TODO."

    if (mram_ok) {
      WriteLatch(latch);
      const LatchReading after = ReadLatch();
      if (after.Deployed()) {
        printf("[antenna] deployed latch set (%u of %u copies)\n", unsigned(after.votes), unsigned(LATCH_COPIES));
      } else {
        printf("[antenna] ERROR: deployed latch did not take (%u of %u copies); the burn will repeat next boot\n",
               unsigned(after.votes), unsigned(LATCH_COPIES));
        PrintLatch(after);
      }
    }

    Retire();
  }

} // namespace OwlSat
