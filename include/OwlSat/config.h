/**

  @file       config.h
  @brief      Compile-time configuration for the OwlSatOS task set.
  @details    Cadences, priorities, stack depths and buffer sizes for the four flight tasks.
              Every number the scheduler or the storage table depends on lives here, so that
              retuning the system is a one-file change and so that the watchdog deadlines can
              be derived from the periods they are meant to police rather than guessed at
              independently.

              Priorities are bounded by configMAX_PRIORITIES (5, so 0..4) and must stay clear of
              configTIMER_TASK_PRIORITY (3) unless the timer service is meant to be preempted.

  @author     Viola Case
  @date       29.08.2026
  @copyright  © Viola Case, 2026. All rights reserved.

**/
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace OwlSat::Config {

  // ---------------------------------------------------------------------------
  // Array geometry
  //
  // Mirrors SXUV5_FACE_COUNT from the sensor branch. Declared independently here so this
  // branch builds without the hardware headers; the two must agree at merge time.
  // ---------------------------------------------------------------------------

  /// Populated EUV diode faces. The sixth face carries the antenna.
  inline constexpr size_t UV_FACE_COUNT = 5;


  // ---------------------------------------------------------------------------
  // Cadences [ms]
  // ---------------------------------------------------------------------------

  /**
   * Science sampling period.
   *
   * A pass across the array is sequential — one mux and one TIA serve all five diodes — so a
   * pass costs five channel changes and their settling delays, up to ~25 ms at the highest
   * gain. Multi-second cadence keeps the duty cycle low and matches what the science asks for.
   */
  inline constexpr uint32_t SENSOR_PERIOD_MS = 5000;

  /// How often the link task asks the radio whether it will accept a frame.
  inline constexpr uint32_t LINK_POLL_PERIOD_MS = 1000;

  /// How long the transmit task blocks waiting for the link-ready bit before looping to log.
  inline constexpr uint32_t TRANSMIT_WAIT_MS = 2000;

  /// Minimum gap between frames handed to the radio, so one pass cannot monopolise the link.
  inline constexpr uint32_t TRANSMIT_GAP_MS = 50;

  /**
   * Watchdog kick period.
   *
   * Must be comfortably shorter than the external circuit's timeout, which is a board-level
   * number and not yet fixed. 250 ms is a placeholder chosen to be short enough for any
   * plausible WDT part.
   */
  inline constexpr uint32_t WATCHDOG_PERIOD_MS = 250;

  /// Onboard-LED heartbeat half-period.
  inline constexpr uint32_t BLINK_HALF_PERIOD_MS = 500;


  // ---------------------------------------------------------------------------
  // Watchdog deadlines [ms]
  //
  // A client that has not checked in within its deadline stops the pulse train, and the external
  // circuit resets the board. Derived from each task's period with margin, rather than written as
  // independent literals, so retuning a period cannot silently arm a spurious reset.
  // ---------------------------------------------------------------------------

  /// Slack allowed on top of a task's nominal period before it counts as late.
  inline constexpr uint32_t WATCHDOG_MARGIN = 3;

  inline constexpr uint32_t WATCHDOG_DEADLINE_SENSOR_MS   = SENSOR_PERIOD_MS * WATCHDOG_MARGIN;
  inline constexpr uint32_t WATCHDOG_DEADLINE_LINK_MS     = LINK_POLL_PERIOD_MS * WATCHDOG_MARGIN;
  inline constexpr uint32_t WATCHDOG_DEADLINE_TRANSMIT_MS = TRANSMIT_WAIT_MS * WATCHDOG_MARGIN;

  /**
   * Grace period after Init() before deadlines are enforced [ms].
   *
   * Tasks have not run yet when the scheduler starts, and the transmit task in particular may
   * legitimately sit blocked through its first deadline. Without this the board would reset
   * itself during boot.
   */
  inline constexpr uint32_t WATCHDOG_BOOT_GRACE_MS = 10000;


  // ---------------------------------------------------------------------------
  // Task priorities  (0 = idle .. configMAX_PRIORITIES-1 = 4)
  //
  //   4  unused — reserved for a future fault handler
  //   3  FreeRTOS timer service (configTIMER_TASK_PRIORITY)
  //   2  watchdog, transmit, antenna deployment
  //   1  sensor, link, blink
  //   0  idle, and the antenna deployment task once the antennae are deployed
  //
  // The watchdog does NOT run at the top priority. A kicker that outranks everything proves only
  // that the scheduler still switches to the highest-priority ready task; running it alongside the
  // work it polices, and gating the pulse on check-ins from the tasks below it, is what makes the
  // pulse train evidence that the system as a whole is still making progress.
  // ---------------------------------------------------------------------------

  inline constexpr uint32_t PRIO_WATCHDOG = 2;
  inline constexpr uint32_t PRIO_TRANSMIT = 2;
  inline constexpr uint32_t PRIO_SENSOR   = 1;
  inline constexpr uint32_t PRIO_LINK     = 1;
  inline constexpr uint32_t PRIO_BLINK    = 1;

  /// Starting priority only. The task drops itself to 0 once the deployed latch is set.
  inline constexpr uint32_t PRIO_ANTENNA  = 2;


  // ---------------------------------------------------------------------------
  // Stack depths [words, not bytes]
  //
  // Generous while the tasks still printf(); newlib's formatted output is the single largest
  // stack consumer in any of them. Re-measure with uxTaskGetStackHighWaterMark() before trimming.
  // ---------------------------------------------------------------------------

  inline constexpr uint16_t STACK_SENSOR   = 1024;
  inline constexpr uint16_t STACK_LINK     = 512;
  inline constexpr uint16_t STACK_TRANSMIT = 1024;
  inline constexpr uint16_t STACK_WATCHDOG = 512;
  inline constexpr uint16_t STACK_BLINK    = 256;
  inline constexpr uint16_t STACK_ANTENNA  = 1024; ///< The MRAM transfer belt (256 B) lives on this stack.


  // ---------------------------------------------------------------------------
  // Storage table
  // ---------------------------------------------------------------------------

  /**
   * Records held in the in-RAM storage table.
   *
   * At SENSOR_PERIOD_MS this is a little over 21 minutes of science with the downlink shut.
   * The table is the volatile front of the store: it exists so the sensor task never blocks on the
   * filesystem and so the transmit path has something to read that is not the flash. Nonvolatile
   * retention is the storage branch's job — see Hal::StorageAppend().
   */
  inline constexpr size_t TABLE_CAPACITY = 256;


  // ---------------------------------------------------------------------------
  // Downlink framing
  // ---------------------------------------------------------------------------

  /// Largest payload carried in one frame [bytes]. Provisional — see telemetry.h.
  inline constexpr size_t PACKET_MAX_PAYLOAD = 220;

  /// Frames handed to the radio in a single transmit pass, before yielding.
  inline constexpr uint32_t PACKET_BURST_LIMIT = 8;


  // ---------------------------------------------------------------------------
  // Antenna deployment
  //
  // The antenna-deployed latch is one 32-bit word, stored as several independent copies in the
  // MRAM. A copy counts as "deployed" if it is within ANTENNA_LATCH_MAX_BIT_ERRORS bits of
  // ANTENNA_LATCH_DEPLOYED, and the latch is set if a majority of copies count. So:
  //   - a set latch survives any single bad bit — in fact up to MAX_BIT_ERRORS bad bits in every
  //     copy, or one copy destroyed outright;
  //   - a clear latch cannot be read as set by a single bad bit, because erased or random MRAM
  //     sits ~16 bits away from the deployed word and a majority of copies would have to drift.
  // The second direction matters more: a falsely-set latch means the antennae never deploy, while
  // a falsely-clear one only re-fires a burn wire that has already done its job.
  //
  // Placement is provisional until the storage layer exists. The copies sit in the top half of the
  // AS3016204, which the README reserves for storage, well clear of the running image at the
  // bottom and of the bench write test at 0x1FF000, and in separate 256 KiB chunks so that one
  // localised fault cannot take out a majority. The storage layer must not hand these words out.
  // ---------------------------------------------------------------------------

  /**
   * How long BURN_0 is held high [ms].
   *
   * TBC — depends on the burn wire, its switch and VBATT. 5 s is a typical nichrome figure and a
   * placeholder only.
   */
  inline constexpr uint32_t ANTENNA_BURN_MS = 5000;

  /// MRAM byte address of each copy of the deployed latch. Odd count, at least three.
  inline constexpr std::array<uint32_t, 3> ANTENNA_LATCH_ADDRESSES = { 0x100000, 0x140000, 0x180000 };

  /**
   * The word a copy holds once the antennae are deployed.
   *
   * 16 bits set and 16 clear, so it is as far as a 32-bit word can be from both 0x00000000 and
   * 0xFFFFFFFF, the two most likely contents of MRAM nobody has written.
   */
  inline constexpr uint32_t ANTENNA_LATCH_DEPLOYED = 0xA5C396E1;

  /// Bit errors a copy may carry and still count as deployed. Must be well under half of 16.
  inline constexpr unsigned ANTENNA_LATCH_MAX_BIT_ERRORS = 4;

} // namespace OwlSat::Config
