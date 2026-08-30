/**

  @file       ltm1_link.h
  @brief      Radio-branch entry points that sit alongside the hal.h radio facade.
  @details    hal.h names the three calls the task layer needs from a radio — init, ask, send —
              and deliberately says nothing about the LTM. That is the right surface for the
              tasks and the wrong one for two things this branch has to expose:

                PollInbound()   the LTM announces its operational mode over the bus rather than
                                answering a query, so somebody has to drain the receive queue.
                                Mode changes the meaning of "ready", so the link task calls this.

                PublishHealth() ICD Table 8 telemetry is host state, not a downlink frame. There
                                is nothing to derive it from in a call that takes frame bytes.

              @par Why health is not routed through Hal::RadioSendPacket()
              A HealthSnapshot is not a function of an OwlSatFrame — it comes from the power
              monitors, the thermistors and the GPS, all of which live on branches that have not
              merged. Reaching for it inside RadioSendPacket() would mean inventing a data source
              to satisfy an interface, which is the one thing hal_stub.cpp exists to avoid. So
              health gets its own call, no caller on this branch yet, and an honest zero return
              until something real fills a snapshot in.

  @author     Viola Case
  @date       29.08.2026
  @copyright  (c) Viola Case, 2026. All rights reserved.

**/
#pragma once

#include <cstddef>

#include <OwlSat/ltm1.h>

namespace OwlSat::Ltm1 {

  /**
   * @brief Drains the receive queue and acts on anything the LTM addressed to this host.
   *
   * Handles Table 7 status messages, which is how the LTM's operational mode reaches us. Opaque
   * uplinked commands are recognised and counted but not yet dispatched — command handling is a
   * separate task on the GANTT and wiring it in from here would bury it.
   *
   * Safe to call when the bus is down; it does nothing and reports nothing.
   *
   * @return Messages consumed.
   */
  size_t PollInbound();

  /// @return The LTM's operational mode as last announced, or Unknown before it has said.
  LtmMode Mode();

  /**
   * @brief Asks the LTM to change mode. A request, not a command — the LTM owns the mode.
   *
   * The host is permitted to ask for safe mode when it sees a condition the LTM cannot, a low
   * battery being the obvious one. Confirmation arrives as an inbound status message; Mode()
   * does not change until it does.
   *
   * @return False if the request could not be queued.
   */
  bool RequestMode(StatusMsg request, uint8_t reason);

  /**
   * @brief Emits @p snap as ICD Table 8 health telemetry (path B).
   *
   * @return Messages actually queued. Zero when the bus is down, when OWLSAT_LTM_SEND_HEALTH is
   *         off, or when the snapshot holds no valid group at all.
   */
  size_t PublishHealth(const HealthSnapshot &snap);

  /// Counters for the console and for whatever health reporting eventually consumes them.
  struct Stats {
    uint32_t science_frames;   ///< Frames accepted by RadioSendPacket() and fully queued.
    uint32_t science_messages; ///< CAN messages queued for those frames.
    uint32_t health_messages;  ///< Table 8 messages queued.
    uint32_t rx_status;        ///< Inbound Table 7 status messages acted on.
    uint32_t rx_opaque;        ///< Inbound messages for this host that are not status.
    uint32_t rx_ignored;       ///< Inbound messages addressed elsewhere (destination below 8).
    uint32_t tx_refused;       ///< Messages the controller would not take.
  };

  /// @return A snapshot of the counters above.
  Stats GetStats();

} // namespace OwlSat::Ltm1
