/**

  @file       tasks.h
  @brief      Entry points for the OwlSatOS flight tasks.
  @details    Four tasks make up the barebones system, plus the watchdog kicker declared in
              watchdog.h:

                SensorTask    — acquires the EUV array on a fixed cadence, writes the storage table
                LinkTask      — asks the radio whether it will accept frames, publishes the answer
                TransmitTask  — packs pending records into frames and hands them to the radio
                WatchdogTask  — pulses WDT_WDI while every task above is still checking in

              @par How the transmit path is split
              Asking "can we transmit?" and "transmit this" are separate tasks because they fail
              separately and want different cadences. The query is a cheap poll against a radio
              that may be powered down; the transmit is a blocking burst that must not start
              unless the answer was recently yes. The link task publishes readiness as an event
              group bit and the transmit task blocks on it, so with the radio absent — as it is on
              this branch — the transmit task sits idle rather than spinning against a dead link.

  @author     Viola Case
  @date       29.08.2026
  @copyright  © Viola Case, 2026. All rights reserved.

**/
#pragma once

#include "FreeRTOS.h"
#include "event_groups.h"

namespace OwlSat {

  /// Set by LinkTask while the radio will accept at least one frame; cleared otherwise.
  constexpr EventBits_t EVT_LINK_READY = 1u << 0;

  /// Set once by TransmitTask after its first successful frame. Diagnostic only.
  constexpr EventBits_t EVT_FIRST_DOWNLINK = 1u << 1;

  /**
   * @brief Creates the event group the link and transmit tasks share.
   *
   * Call before the scheduler starts and before either task is created.
   *
   * @return False if the group could not be allocated.
   */
  bool InitTaskEvents();

  /// @return The shared event group, or nullptr before InitTaskEvents() succeeds.
  EventGroupHandle_t TaskEvents();

  /**
   * @brief Requests data from the UV sensor array and writes it to the storage table.
   *
   * Paced with xTaskDelayUntil against a fixed reference so the cadence does not drift with the
   * length of a pass. A failed acquisition is logged and skipped — it does not stall the
   * cadence, and it does not write a fabricated row.
   *
   * @param params Unused (pass nullptr).
   */
  void SensorTask(void *params);

  /**
   * @brief Queries whether the radio is ready to transmit, and publishes the answer.
   *
   * Polls Hal::RadioQueryReady() and maintains EVT_LINK_READY. Nothing else acts on the radio
   * here — this task only ever asks.
   *
   * @param params Unused (pass nullptr).
   */
  void LinkTask(void *params);

  /**
   * @brief Transmits pending records as packets, once the link says it is ready.
   *
   * Waits on EVT_LINK_READY, pulls pending records from the storage table, packs them into
   * TelemetryBatch frames and hands them to the radio. Records are marked downlinked only after
   * the radio accepts the frame carrying them; a rejected frame leaves them pending and ends the
   * pass, so a link that fails mid-burst costs a retry and not the data.
   *
   * @param params Unused (pass nullptr).
   */
  void TransmitTask(void *params);

} // namespace OwlSat
