/**
 * @file storage_table.cpp
 * @brief Implementation of the in-RAM storage table.
 */

#include <cstring>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include <OwlSat/hal.h>
#include <OwlSat/storage_table.h>

namespace OwlSat::StorageTable {

  namespace {

    /// The ring. Statically allocated — the flight heap is for tasks, not for science buffers.
    TelemetryRecord g_records[OWLSAT_TABLE_CAPACITY];

    SemaphoreHandle_t g_mutex = nullptr;

    size_t   g_head  = 0; ///< Index the next append writes to.
    size_t   g_count = 0; ///< Rows currently valid, <= OWLSAT_TABLE_CAPACITY.
    uint32_t g_next_seq = 1; ///< Sequence numbers start at 1; 0 means "no record".

    /**
     * Every record with seq <= this has been downlinked or dropped.
     *
     * Starting at 0 with sequence numbers starting at 1 means the first record is pending the
     * moment it is written, with no special case for the empty table.
     */
    uint32_t g_sent_thru_seq = 0;

    uint32_t g_appended   = 0;
    uint32_t g_dropped    = 0;
    uint32_t g_downlinked = 0;

    /// Index of the oldest valid row.
    inline size_t OldestIndex() {
      return (g_head + OWLSAT_TABLE_CAPACITY - g_count) % OWLSAT_TABLE_CAPACITY;
    }

    inline bool Lock() {
      return g_mutex != nullptr && xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE;
    }

    inline void Unlock() {
      xSemaphoreGive(g_mutex);
    }

  } // namespace

  bool Init() {
    if (g_mutex != nullptr) {
      return true;
    }
    g_mutex = xSemaphoreCreateMutex();
    if (g_mutex == nullptr) {
      return false;
    }

    std::memset(g_records, 0, sizeof(g_records));
    g_head = 0;
    g_count = 0;
    g_next_seq = 1;
    g_sent_thru_seq = 0;
    g_appended = 0;
    g_dropped = 0;
    g_downlinked = 0;
    return true;
  }

  bool Append(const UvSample &uv, uint32_t *seq_out) {
    if (!Lock()) {
      return false;
    }

    // Full ring: the row about to be overwritten is the oldest one. If the downlink never got
    // to it, that is a real loss and is counted as one — and the cursor is dragged past it so
    // the transmit task is not left hunting for a sequence number that no longer exists.
    if (g_count == OWLSAT_TABLE_CAPACITY) {
      const uint32_t victim_seq = g_records[g_head].seq;
      if (victim_seq > g_sent_thru_seq) {
        ++g_dropped;
        g_sent_thru_seq = victim_seq;
      }
      --g_count;
    }

    TelemetryRecord &row = g_records[g_head];
    row.seq       = g_next_seq++;
    row.uptime_ms = static_cast<uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount()));
    row.uv        = uv;

    // Copied out so the nonvolatile mirror below happens with the mutex released. A flash or
    // MRAM write is orders of magnitude slower than anything else that touches this table, and
    // holding the lock across it would put the transmit task behind the storage driver.
    const TelemetryRecord persisted = row;

    g_head = (g_head + 1) % OWLSAT_TABLE_CAPACITY;
    ++g_count;
    ++g_appended;

    Unlock();

    // Best-effort durability. Returns false on this branch and is expected to: the RAM table is
    // the system of record until the storage branch merges.
    (void) Hal::StorageAppend(persisted);

    if (seq_out != nullptr) {
      *seq_out = persisted.seq;
    }
    return true;
  }

  size_t PeekPending(TelemetryRecord *out, size_t max) {
    if (out == nullptr || max == 0 || !Lock()) {
      return 0;
    }

    size_t written = 0;
    size_t idx = OldestIndex();
    for (size_t i = 0; i < g_count && written < max; ++i) {
      const TelemetryRecord &row = g_records[idx];
      if (row.seq > g_sent_thru_seq) {
        out[written++] = row;
      }
      idx = (idx + 1) % OWLSAT_TABLE_CAPACITY;
    }

    Unlock();
    return written;
  }

  void MarkTransmitted(uint32_t seq) {
    if (!Lock()) {
      return;
    }

    // Guard against a stale or duplicated acknowledgement walking the cursor backwards, which
    // would re-downlink records that have already been sent.
    if (seq > g_sent_thru_seq) {
      // Count the rows actually crossed rather than the width of the sequence span. The two
      // differ whenever a record was dropped from a full ring: that record advanced the cursor
      // without ever reaching the radio, and must not be reported as downlinked.
      size_t idx = OldestIndex();
      for (size_t i = 0; i < g_count; ++i) {
        const uint32_t row_seq = g_records[idx].seq;
        if (row_seq > g_sent_thru_seq && row_seq <= seq) {
          ++g_downlinked;
        }
        idx = (idx + 1) % OWLSAT_TABLE_CAPACITY;
      }
      g_sent_thru_seq = seq;
    }

    Unlock();
  }

  bool HasPending() {
    if (!Lock()) {
      return false;
    }
    const bool pending = (g_next_seq - 1) > g_sent_thru_seq;
    Unlock();
    return pending;
  }

  Stats GetStats() {
    Stats stats = {};
    if (!Lock()) {
      return stats;
    }

    stats.appended      = g_appended;
    stats.dropped       = g_dropped;
    stats.downlinked    = g_downlinked;
    stats.held          = static_cast<uint32_t>(g_count);
    stats.next_seq      = g_next_seq;
    stats.sent_thru_seq = g_sent_thru_seq;

    stats.pending = 0;
    size_t idx = OldestIndex();
    for (size_t i = 0; i < g_count; ++i) {
      if (g_records[idx].seq > g_sent_thru_seq) {
        ++stats.pending;
      }
      idx = (idx + 1) % OWLSAT_TABLE_CAPACITY;
    }

    Unlock();
    return stats;
  }

} // namespace OwlSat::StorageTable
