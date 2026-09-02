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


// ---------------------------------------------------------------------------
// Array geometry
//
// Mirrors SXUV5_FACE_COUNT from the sensor branch. Declared independently here so this
// branch builds without the hardware headers; the two must agree at merge time.
// ---------------------------------------------------------------------------

/// Populated EUV diode faces. The sixth face carries the antenna.
#define OWLSAT_UV_FACE_COUNT 5


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
#define OWLSAT_SENSOR_PERIOD_MS 5000u

/// How often the link task asks the radio whether it will accept a frame.
#define OWLSAT_LINK_POLL_PERIOD_MS 1000u

/// How long the transmit task blocks waiting for the link-ready bit before looping to log.
#define OWLSAT_TRANSMIT_WAIT_MS 2000u

/// Minimum gap between frames handed to the radio, so one pass cannot monopolise the link.
#define OWLSAT_TRANSMIT_GAP_MS 50u

/**
 * Watchdog kick period.
 *
 * Must be comfortably shorter than the external circuit's timeout, which is a board-level
 * number and not yet fixed. 250 ms is a placeholder chosen to be short enough for any
 * plausible WDT part.
 */
#define OWLSAT_WATCHDOG_PERIOD_MS 250u

/// Onboard-LED heartbeat half-period.
#define OWLSAT_BLINK_HALF_PERIOD_MS 500u


// ---------------------------------------------------------------------------
// Watchdog deadlines [ms]
//
// A client that has not checked in within its deadline stops the pulse train, and the external
// circuit resets the board. Derived from each task's period with margin, rather than written as
// independent literals, so retuning a period cannot silently arm a spurious reset.
// ---------------------------------------------------------------------------

/// Slack allowed on top of a task's nominal period before it counts as late.
#define OWLSAT_WATCHDOG_MARGIN 3u

#define OWLSAT_WATCHDOG_DEADLINE_SENSOR_MS   (OWLSAT_SENSOR_PERIOD_MS * OWLSAT_WATCHDOG_MARGIN)
#define OWLSAT_WATCHDOG_DEADLINE_LINK_MS     (OWLSAT_LINK_POLL_PERIOD_MS * OWLSAT_WATCHDOG_MARGIN)
#define OWLSAT_WATCHDOG_DEADLINE_TRANSMIT_MS (OWLSAT_TRANSMIT_WAIT_MS * OWLSAT_WATCHDOG_MARGIN)

/**
 * Grace period after Init() before deadlines are enforced [ms].
 *
 * Tasks have not run yet when the scheduler starts, and the transmit task in particular may
 * legitimately sit blocked through its first deadline. Without this the board would reset
 * itself during boot.
 */
#define OWLSAT_WATCHDOG_BOOT_GRACE_MS 10000u


// ---------------------------------------------------------------------------
// Task priorities  (0 = idle .. configMAX_PRIORITIES-1 = 4)
//
//   4  unused — reserved for a future fault handler
//   3  FreeRTOS timer service (configTIMER_TASK_PRIORITY)
//   2  watchdog, transmit
//   1  sensor, link, blink
//   0  idle
//
// The watchdog does NOT run at the top priority. A kicker that outranks everything proves only
// that the scheduler still switches to the highest-priority ready task; running it alongside the
// work it polices, and gating the pulse on check-ins from the tasks below it, is what makes the
// pulse train evidence that the system as a whole is still making progress.
// ---------------------------------------------------------------------------

#define OWLSAT_PRIO_WATCHDOG 2
#define OWLSAT_PRIO_TRANSMIT 2
#define OWLSAT_PRIO_SENSOR   1
#define OWLSAT_PRIO_LINK     1
#define OWLSAT_PRIO_BLINK    1


// ---------------------------------------------------------------------------
// Stack depths [words, not bytes]
//
// Generous while the tasks still printf(); newlib's formatted output is the single largest
// stack consumer in any of them. Re-measure with uxTaskGetStackHighWaterMark() before trimming.
// ---------------------------------------------------------------------------

#define OWLSAT_STACK_SENSOR   1024
#define OWLSAT_STACK_LINK     512
#define OWLSAT_STACK_TRANSMIT 1024
#define OWLSAT_STACK_WATCHDOG 512
#define OWLSAT_STACK_BLINK    256


// ---------------------------------------------------------------------------
// Storage table
// ---------------------------------------------------------------------------

/**
 * Records held in the in-RAM storage table.
 *
 * At OWLSAT_SENSOR_PERIOD_MS this is a little over 21 minutes of science with the downlink shut.
 * The table is the volatile front of the store: it exists so the sensor task never blocks on the
 * filesystem and so the transmit path has something to read that is not the flash. Nonvolatile
 * retention is the storage branch's job — see Hal::StorageAppend().
 */
#define OWLSAT_TABLE_CAPACITY 256u


// ---------------------------------------------------------------------------
// Downlink framing
// ---------------------------------------------------------------------------

/// Largest payload carried in one frame [bytes]. Provisional — see telemetry.h.
#define OWLSAT_PACKET_MAX_PAYLOAD 220u

/// Frames handed to the radio in a single transmit pass, before yielding.
#define OWLSAT_PACKET_BURST_LIMIT 8u
