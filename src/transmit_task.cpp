/**
 * @file transmit_task.cpp
 * @brief Packs pending records into frames and transmits them.
 */

#include <cstdio>

#include "FreeRTOS.h"
#include "event_groups.h"
#include "task.h"

#include <OwlSat/config.h>
#include <OwlSat/hal.h>
#include <OwlSat/storage_table.h>
#include <OwlSat/tasks.h>
#include <OwlSat/watchdog.h>

namespace OwlSat {

  namespace {

    /**
     * Working buffers, file-static rather than on the stack.
     *
     * Together they are a little over a kilobyte, and a task stack is measured in words. Putting
     * them in .bss keeps OWLSAT_STACK_TRANSMIT honest and keeps a change to
     * OWLSAT_PACKET_MAX_PAYLOAD from quietly turning into a stack overflow. Only this task
     * touches them, so no lock is needed.
     */
    TelemetryRecord g_batch[OWLSAT_RECORDS_PER_FRAME];
    uint8_t         g_frame[OWLSAT_FRAME_MAX_BYTES];

    /// Monotonic frame counter. Independent of record sequence numbers — the ground uses it to
    /// spot frames lost in the link, which record numbers alone cannot distinguish from records
    /// that were never acquired.
    uint32_t g_frame_seq = 1;

    /// How many idle waits pass before the task reports the backlog. At OWLSAT_TRANSMIT_WAIT_MS
    /// this is roughly one line a minute while the link is down.
    constexpr uint32_t IDLE_LOG_INTERVAL = 30;

  } // namespace

  void TransmitTask(void *params) {
    (void) params;

    EventGroupHandle_t events = TaskEvents();
    configASSERT(events != nullptr);

    printf("[tx] ready; %u records per frame, %u byte frames max\n",
           (unsigned) OWLSAT_RECORDS_PER_FRAME,
           (unsigned) OWLSAT_FRAME_MAX_BYTES);

    uint32_t idle_waits = 0;

    for (;;) {
      Watchdog::CheckIn(Watchdog::Client::Transmit);

      // Block until the link task says the radio will take a frame. Waiting on the bit rather
      // than polling the radio here is what keeps the two concerns apart: this task never asks
      // whether it may transmit, only how. The timeout exists so the task still checks in with
      // the watchdog and still reports its backlog while the link is down.
      const EventBits_t bits = xEventGroupWaitBits(events,
                                                   EVT_LINK_READY,
                                                   pdFALSE,  // leave the bit for the next pass
                                                   pdTRUE,   // wait for all bits in the set
                                                   pdMS_TO_TICKS(OWLSAT_TRANSMIT_WAIT_MS));

      if ((bits & EVT_LINK_READY) == 0) {
        if (idle_waits % IDLE_LOG_INTERVAL == 0) {
          const StorageTable::Stats stats = StorageTable::GetStats();
          printf("[tx] link not ready; %lu pending, %lu dropped, %lu downlinked\n",
                 (unsigned long) stats.pending,
                 (unsigned long) stats.dropped,
                 (unsigned long) stats.downlinked);
        }
        ++idle_waits;
        continue;
      }
      idle_waits = 0;

      // One burst. Bounded so that a large backlog cannot hold the CPU for an unbounded stretch
      // and cannot outrun the link's own idea of how many frames it will take — the link task
      // re-polls between bursts and will clear the bit if the radio pushes back.
      uint32_t frames_sent = 0;
      for (uint32_t i = 0; i < OWLSAT_PACKET_BURST_LIMIT; ++i) {
        const size_t count = StorageTable::PeekPending(g_batch, OWLSAT_RECORDS_PER_FRAME);
        if (count == 0) {
          break;
        }

        const size_t len = BuildTelemetryFrame(g_frame_seq, g_batch, count, g_frame);
        if (len == 0) {
          printf("[tx] ERROR: framing produced an empty packet for %u records\n",
                 (unsigned) count);
          break;
        }

        if (!Hal::RadioSendPacket(g_frame, len)) {
          // The frame was not accepted, so the records in it are still pending. End the burst
          // rather than retry immediately: the link task will notice on its next poll and clear
          // EVT_LINK_READY, and hammering a radio that just refused a frame is how a marginal
          // link becomes a busy loop.
          printf("[tx] frame %lu rejected (%u records, %u bytes); %u still pending\n",
                 (unsigned long) g_frame_seq,
                 (unsigned) count,
                 (unsigned) len,
                 (unsigned) count);
          break;
        }

        // Only now are these records downlinked. Marking them before the radio accepted the
        // frame would turn every rejected frame into permanently lost science.
        StorageTable::MarkTransmitted(g_batch[count - 1].seq);
        xEventGroupSetBits(events, EVT_FIRST_DOWNLINK);

        printf("[tx] frame %lu sent: %u records (seq %lu..%lu), %u bytes\n",
               (unsigned long) g_frame_seq,
               (unsigned) count,
               (unsigned long) g_batch[0].seq,
               (unsigned long) g_batch[count - 1].seq,
               (unsigned) len);

        ++g_frame_seq;
        ++frames_sent;

        // Let the radio drain and let lower-priority work run. The LTM-1 is behind a CAN bridge
        // and will not swallow frames back to back at this task's speed.
        vTaskDelay(pdMS_TO_TICKS(OWLSAT_TRANSMIT_GAP_MS));
      }

      if (frames_sent == 0) {
        // Link is up but there is nothing to send. Sleep rather than spin on a set event bit.
        vTaskDelay(pdMS_TO_TICKS(OWLSAT_TRANSMIT_WAIT_MS));
      }
    }
  }

} // namespace OwlSat
