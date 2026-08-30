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


// ---------------------------------------------------------------------------
// AMSAT LTM-1 link  (see include/OwlSat/ltm1.h and docs/internal/ltm1_link_design.md)
//
// Only the values the ICD leaves to the host are here. The destination field, the LTM's own
// source id and the Table 7/8 message ids are fixed by the ICD and live as constants in ltm1.h
// — putting them here would suggest they are ours to tune, and they are not.
// ---------------------------------------------------------------------------

/**
 * CAN identifier source field for this host (ICD Table 6, bits 23:20).
 *
 * The ICD requires 8 or above whenever the destination is the LTM, and leaves the specific
 * value to the host. 8 is the bottom of the permitted range; nothing else shares this bus.
 */
#define OWLSAT_LTM_SOURCE_ID 8u

/**
 * Bus bit rate [bit/s]. Fixed by the ICD at 125k, not a tuning knob — it is here so the CAN
 * controller driver has one place to read it from rather than a literal in a bit-timing table.
 */
#define OWLSAT_LTM_CAN_BITRATE 125000u

/**
 * Priority fields (ICD Table 6, bits 28:24). Lower wins CAN arbitration.
 *
 * Both sides are required to accept any priority and to ignore it when interpreting a message,
 * so these only order our own traffic against itself. Mode coordination outranks health, and
 * health outranks bulk science: losing a science chunk to arbitration costs one frame, losing a
 * safe-mode request costs the power margin it was asking for.
 */
#define OWLSAT_LTM_PRIORITY_STATUS  1u
#define OWLSAT_LTM_PRIORITY_HEALTH  8u
#define OWLSAT_LTM_PRIORITY_SCIENCE 16u

/**
 * Type field for OwlSat science (ICD Table 6, bits 19:16). Default 10 = health-mode realtime
 * plus Whole Orbit Data.
 *
 * The alternative worth knowing about is 2 — realtime only, no WOD. Realtime alone means science
 * acquired outside a ground station pass is downlinked into an empty sky and gone, which is
 * exactly the loss the storage table upstream exists to prevent; carrying it into the LTM's WOD
 * buffer keeps it alive across passes. The cost is LTM buffer the ICD marks TBS, so if AMSAT
 * comes back with a tight WOD budget this is the macro that changes.
 *
 * Values are OwlSat::Ltm1::MsgType. Not an enum here because config.h predates that header.
 */
#define OWLSAT_LTM_SCIENCE_TYPE 10u

/// Emit science as opaque chunked OwlSatFrames (path A). See ltm1.h.
#define OWLSAT_LTM_SEND_SCIENCE 1

/// Emit host state as ICD Table 8 health telemetry for FoxTelem (path B). See ltm1.h.
#define OWLSAT_LTM_SEND_HEALTH 1

/**
 * Messages the link layer hands the controller in one call before yielding.
 *
 * A full frame is SCIENCE_MAX_MESSAGES chunks and the controller has only a few TX mailboxes,
 * so a burst larger than this just spins on a full queue.
 */
#define OWLSAT_LTM_TX_BURST 8u

/**
 * Messages drained from the receive queue per link-task poll.
 *
 * A bound, not a target. The LTM speaks rarely — mode changes and passed-through commands — so
 * this is normally never reached; it exists so that a controller wedged reporting a permanently
 * non-empty queue cannot hold the link task past its watchdog deadline.
 */
#define OWLSAT_LTM_RX_BURST 8u
