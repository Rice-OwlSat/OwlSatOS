/**
 * @file link_task.cpp
 * @brief Queries whether the radio is ready to transmit and publishes the answer.
 */

#include <cstdio>

#include "FreeRTOS.h"
#include "event_groups.h"
#include "task.h"

#include <OwlSat/config.h>
#include <OwlSat/hal.h>
#include <OwlSat/ltm1_link.h>
#include <OwlSat/storage_table.h>
#include <OwlSat/tasks.h>
#include <OwlSat/watchdog.h>

namespace OwlSat {

  namespace {

    const char *LinkStateName(Hal::LinkState state) {
      switch (state) {
        case Hal::LinkState::Down:     return "down";
        case Hal::LinkState::NotReady: return "not-ready";
        case Hal::LinkState::Ready:    return "ready";
        case Hal::LinkState::Unknown:  break;
      }
      return "unknown";
    }

  } // namespace

  void LinkTask(void *params) {
    (void) params;

    const bool radio_up = Hal::RadioInit();
    printf("[link] radio %s; polling readiness every %lu ms\n",
           radio_up ? "up" : "DOWN (will keep asking)",
           (unsigned long) OWLSAT_LINK_POLL_PERIOD_MS);

    EventGroupHandle_t events = TaskEvents();
    configASSERT(events != nullptr);

    // Seeded to Ready so the first poll is always treated as a change and logged, whatever it
    // finds. Without this a radio that is down from boot would never announce itself.
    Hal::LinkState last_state = Hal::LinkState::Ready;

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(OWLSAT_LINK_POLL_PERIOD_MS);

    for (;;) {
      Watchdog::CheckIn(Watchdog::Client::Link);

      // Drain the bus before asking. The LTM announces its operational mode rather than
      // answering questions about it, and mode is one of the three things readiness is derived
      // from — so a status message left sitting in the receive queue is a stale answer below.
      Ltm1::PollInbound();

      Hal::LinkStatus status = {};
      const bool queried = Hal::RadioQueryReady(&status);

      // A query that failed is not a link that is ready. Treating an unanswered radio as usable
      // would hand frames to a subsystem that cannot say whether it took them, and the transmit
      // task would mark records downlinked that never left the spacecraft.
      const bool ready = queried && status.state == Hal::LinkState::Ready && status.frames_free > 0;

      if (ready) {
        xEventGroupSetBits(events, EVT_LINK_READY);
      } else {
        xEventGroupClearBits(events, EVT_LINK_READY);
      }

      // Edge-triggered logging. This poll runs once a second for the life of the mission; a line
      // per poll would be the only thing on the console.
      if (status.state != last_state) {
        const StorageTable::Stats stats = StorageTable::GetStats();
        printf("[link] %s -> %s (frames_free=%u, %lu records pending)\n",
               LinkStateName(last_state),
               LinkStateName(status.state),
               (unsigned) status.frames_free,
               (unsigned long) stats.pending);
        last_state = status.state;
      }

      xTaskDelayUntil(&last_wake, period);
    }
  }

} // namespace OwlSat
