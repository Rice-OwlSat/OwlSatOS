/**
 * @file ltm1_link.cpp
 * @brief The AMSAT LTM-1 link: implements the hal.h radio facade over the CAN protocol layer.
 *
 * Three layers meet in this file. Above it, hal.h and a task layer that knows nothing about CAN.
 * Below it, can_controller.h and a part that has not been chosen. In it, the policy the ICD
 * implies but does not state: when a frame is worth sending, what "ready" means on a bus that
 * offers no queue-depth query, and what to do about a radio whose operational mode decides
 * whether our science is downlinked at all.
 *
 * @par Replaces the radio section of hal_stub.cpp
 * The UV, storage and watchdog stubs there are untouched. Only the four radio functions moved,
 * and their signatures did not change — the task layer does not know this file exists.
 */

#include <cstdio>

#include "FreeRTOS.h"
#include "task.h"

#include <OwlSat/can_controller.h>
#include <OwlSat/config.h>
#include <OwlSat/hal.h>
#include <OwlSat/ltm1.h>
#include <OwlSat/ltm1_link.h>

namespace OwlSat::Ltm1 {

  namespace {

    /// Scratch for one frame's worth of chunks. See the note on the transmit task's buffers:
    /// SCIENCE_MAX_MESSAGES CanMessages is around 400 bytes, which does not belong on a stack
    /// measured in words. Only Hal::RadioSendPacket() touches it, and only from the tx task.
    CanMessage g_chunks[SCIENCE_MAX_MESSAGES];

    /// Scratch for one health emission. Separate from g_chunks because PublishHealth() is called
    /// from a different task than the transmit path and sharing would need a lock to say so.
    CanMessage g_health[HEALTH_MAX_MESSAGES];

    LtmMode g_mode = LtmMode::Unknown;

    /**
     * Diagnostic counters.
     *
     * Touched from two tasks — the link task through PollInbound(), the transmit task through
     * RadioSendPacket() — and deliberately not locked. The increments are not atomic on this
     * core, so a simultaneous update to tx_refused, the one field both paths write, can lose a
     * count. That is the right trade here: these exist to be printed, a lost count changes
     * nothing anyone acts on, and taking a mutex on the transmit path to protect a statistic
     * would put a blocking call in the one place that has to stay predictable. Do not grow a
     * control decision out of these without revisiting that.
     */
    Stats g_stats = {};

    /// Set once Hal::RadioInit() has run, so readiness reports a state rather than a guess.
    bool g_initialised = false;

  } // namespace


  // -----------------------------------------------------------------------
  // Inbound
  // -----------------------------------------------------------------------

  size_t PollInbound() {
    size_t consumed = 0;

    // Bounded. A controller stuck reporting a permanently non-empty queue must not hold the link
    // task past its watchdog deadline.
    for (size_t i = 0; i < OWLSAT_LTM_RX_BURST; ++i) {
      CanMessage msg = {};
      if (!Can::Receive(&msg)) {
        break;
      }
      ++consumed;

      // ICD Table 6: anything below destination 8 belongs to the LTM or another host.
      if (!IsForHost(msg.id)) {
        ++g_stats.rx_ignored;
        continue;
      }

      const CanIdFields fields = UnpackId(msg.id);

      if (fields.type != MsgType::Status) {
        // An opaque uplinked command, passed through by the LTM. Counted so the console can show
        // that the uplink works at all; dispatch belongs to the command-handling task.
        ++g_stats.rx_opaque;
        continue;
      }

      const LtmMode announced = ModeFromStatus(static_cast<StatusMsg>(fields.msg_id));
      ++g_stats.rx_status;

      if (announced != LtmMode::Unknown && announced != g_mode) {
        const uint8_t reason = msg.dlc > 0 ? msg.data[0] : 0xFF;
        printf("[ltm] mode %u -> %u (reason %u)\n", (unsigned) g_mode, (unsigned) announced,
               (unsigned) reason);
        g_mode = announced;
      }
    }

    return consumed;
  }

  LtmMode Mode() { return g_mode; }

  bool RequestMode(StatusMsg request, uint8_t reason) {
    if (!Can::Available()) {
      return false;
    }
    const CanMessage msg = BuildStatusRequest(request, reason);
    if (!Can::Send(msg)) {
      ++g_stats.tx_refused;
      return false;
    }
    return true;
  }


  // -----------------------------------------------------------------------
  // Health telemetry  (path B)
  // -----------------------------------------------------------------------

  size_t PublishHealth(const HealthSnapshot &snap) {
  #if OWLSAT_LTM_SEND_HEALTH
    if (!Can::Available()) {
      return 0;
    }

    const size_t count = BuildHealthMessages(snap, g_health, HEALTH_MAX_MESSAGES);

    size_t queued = 0;
    for (size_t i = 0; i < count; ++i) {
      if (!Can::Send(g_health[i])) {
        // Health is periodic and idempotent — the next snapshot supersedes this one — so a
        // refused message is dropped rather than retried. Science is the opposite and is
        // handled the opposite way in RadioSendPacket().
        ++g_stats.tx_refused;
        break;
      }
      ++queued;
    }

    g_stats.health_messages += static_cast<uint32_t>(queued);
    return queued;
  #else
    (void) snap;
    return 0;
  #endif
  }

  Stats GetStats() { return g_stats; }

} // namespace OwlSat::Ltm1


namespace OwlSat::Hal {

  bool RadioInit() {
    // MERGE: assert RADIO_PWR before touching the controller — the transceiver is inside the
    // switched domain, and a controller brought up against a dead transceiver reports a healthy
    // bus with nothing on it. pin_assignment.h has no RADIO_PWR macro yet.
    //
    // The ICD also makes this a safety interlock, not just a power step: the host must withhold
    // LTM power until the antennas are deployed and the launch provider's post-release timer has
    // expired. Whatever owns deployment has to gate this call. See the design doc.
    const bool up = Can::Init();

    Ltm1::g_initialised = true;

    printf("[ltm] radio %s; source %u -> dest %u, %lu bit/s, science type %u\n",
           up ? "up" : "DOWN",
           (unsigned) OWLSAT_LTM_SOURCE_ID,
           (unsigned) Ltm1::DEST_LTM,
           (unsigned long) OWLSAT_LTM_CAN_BITRATE,
           (unsigned) OWLSAT_LTM_SCIENCE_TYPE);

    return up;
  }

  bool RadioQueryReady(LinkStatus *out) {
    // Readiness is derived here, not asked. The ICD gives the host no way to query the LTM's
    // transmit queue depth — the bus carries telemetry toward the radio and status back, and
    // nothing resembling a "may I send" round trip. So the three things we can actually know
    // stand in for it, and the design doc says so explicitly rather than leaving the tx task to
    // infer that frames_free means mailboxes and not downlink slots.
    const bool     available = Can::Available();
    const uint16_t mailboxes = available ? Can::TxFree() : 0;
    const bool     mode_ok   = ModeCarriesScience(Ltm1::g_mode);

    if (out != nullptr) {
      out->uptime_ms   = static_cast<uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount()));
      out->frames_free = 0;

      if (!Ltm1::g_initialised) {
        out->state = LinkState::Unknown;
      } else if (!available) {
        // Down, not Unknown: RadioInit() has run, so the controller's silence is an established
        // fact about the hardware and not an untested assumption.
        out->state = LinkState::Down;
      } else if (!mode_ok || mailboxes == 0) {
        // Powered and answering, but this is not the moment. Safe mode is the interesting case:
        // the LTM is shedding load and carries health data only, so science handed over now is
        // buffered at best. Holding it in the storage ring keeps it recoverable.
        out->state = LinkState::NotReady;
      } else {
        out->state = LinkState::Ready;
        // Mailboxes, not frames. One frame is many messages, so this is a floor on how much the
        // bus will take rather than a count of frames — the transmit task only compares it
        // against zero, and the design doc records the mismatch so it stays that way.
        out->frames_free = mailboxes;
      }
    }

    return available;
  }

  bool RadioSendPacket(const uint8_t *frame, size_t len) {
  #if OWLSAT_LTM_SEND_SCIENCE
    if (frame == nullptr || len == 0 || !Can::Available()) {
      return false;
    }

    const size_t count = Ltm1::ChunkFrameToScience(frame, len, Ltm1::g_chunks,
                                                   Ltm1::SCIENCE_MAX_MESSAGES);
    if (count == 0) {
      printf("[ltm] ERROR: %u byte frame did not chunk (limit %u messages)\n",
             (unsigned) len,
             (unsigned) Ltm1::SCIENCE_MAX_MESSAGES);
      return false;
    }

    // All of the frame or none of it. A frame that goes out half-queued reaches the ground as a
    // CRC failure and costs the records in it anyway, so a mailbox that fills mid-frame is
    // reported as a rejection — the transmit task then leaves those records pending and retries,
    // which is exactly what it already does with a radio that pushes back.
    if (Can::TxFree() < count) {
      return false;
    }

    for (size_t i = 0; i < count; ++i) {
      if (!Can::Send(Ltm1::g_chunks[i])) {
        ++Ltm1::g_stats.tx_refused;
        return false;
      }
    }

    ++Ltm1::g_stats.science_frames;
    Ltm1::g_stats.science_messages += static_cast<uint32_t>(count);
    return true;
  #else
    (void) frame;
    (void) len;
    return false;
  #endif
  }

} // namespace OwlSat::Hal
