/**

  @file       storage_table.h
  @brief      The storage table — the in-RAM record store the science and downlink paths share.
  @details    A fixed-capacity ring of TelemetryRecord with a monotonic sequence number per row
              and a cursor marking how far the downlink has got. The sensor task appends to it;
              the transmit task reads from it and marks rows sent.

              @par Why a ring and not a queue
              A FreeRTOS queue would couple the two tasks: with the link down the sensor task
              would eventually block on a full queue and stop sampling, which is the wrong
              failure. Science acquisition must not depend on the radio. A ring drops the
              *oldest* record when it fills, so a long downlink outage costs the beginning of
              the backlog and never a missed sample. Drops are counted, not hidden — see Stats().

              @par Relationship to nonvolatile storage
              This table is the volatile front of the store, not a replacement for it. Each
              append is also offered to Hal::StorageAppend(); when the storage branch merges,
              that call is what makes a record survive a reset, and this table becomes what keeps
              the sensor task off the filesystem's latency.

              @par Threading
              Every entry point takes the table mutex. Safe from any task; not safe from an ISR.

  @author     Viola Case
  @date       29.08.2026
  @copyright  © Viola Case, 2026. All rights reserved.

**/
#pragma once

#include <cstddef>
#include <cstdint>

#include <OwlSat/telemetry.h>

namespace OwlSat::StorageTable {

  /// Counters describing what the table has been through. All monotonic except @c held.
  struct Stats {
    uint32_t appended;      ///< Records ever appended.
    uint32_t dropped;       ///< Records overwritten before they were downlinked.
    uint32_t downlinked;    ///< Records marked transmitted.
    uint32_t held;          ///< Records currently in the ring.
    uint32_t pending;       ///< Of those, how many are not yet downlinked.
    uint32_t next_seq;      ///< Sequence number the next append will take.
    uint32_t sent_thru_seq; ///< Every record with seq <= this has been downlinked.
  };

  /**
   * @brief Creates the table mutex and zeroes the ring.
   *
   * Must be called before the scheduler starts, and before any task touches the table.
   *
   * @return False if the mutex could not be created, which is fatal — the caller should not
   *         start the scheduler.
   */
  bool Init();

  /**
   * @brief Appends one sample, assigning it the next sequence number.
   *
   * When the ring is full the oldest row is overwritten. If that row had not been downlinked it
   * is counted in Stats::dropped and the sent-through cursor is advanced past it, so the
   * transmit task is never left waiting on a record that no longer exists.
   *
   * @param uv      The acquisition pass to store.
   * @param seq_out Optional; receives the sequence number assigned.
   * @return False only if the table was not initialised.
   */
  bool Append(const UvSample &uv, uint32_t *seq_out = nullptr);

  /**
   * @brief Copies out the oldest records that have not been downlinked yet.
   *
   * Copies rather than lends: the ring is mutable under the caller's feet, and a reference into
   * it would be a reference the sensor task can overwrite mid-transmission.
   *
   * @param out      Destination array.
   * @param max      Capacity of @p out.
   * @return Number of records written to @p out, oldest first.
   */
  size_t PeekPending(TelemetryRecord *out, size_t max);

  /**
   * @brief Marks every record up to and including @p seq as downlinked.
   *
   * Called only after the radio has accepted the frame carrying them. Moving the cursor before
   * that would turn a rejected frame into permanently lost science.
   *
   * @param seq Highest sequence number successfully transmitted.
   */
  void MarkTransmitted(uint32_t seq);

  /// @return True if any record is waiting to be downlinked.
  bool HasPending();

  /// @return A consistent snapshot of the counters.
  Stats GetStats();

} // namespace OwlSat::StorageTable
