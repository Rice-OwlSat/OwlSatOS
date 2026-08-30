/**
 * @file sensor_task.cpp
 * @brief Requests data from the UV sensor array and writes it to the storage table.
 */

#include <cstdio>

#include "FreeRTOS.h"
#include "task.h"

#include <OwlSat/config.h>
#include <OwlSat/hal.h>
#include <OwlSat/storage_table.h>
#include <OwlSat/tasks.h>
#include <OwlSat/watchdog.h>

namespace OwlSat {

  namespace {
    /// Consecutive failures before the task complains again, so a dead sensor logs once a minute
    /// rather than once a pass.
    constexpr uint32_t FAILURE_LOG_INTERVAL = 12;
  } // namespace

  void SensorTask(void *params) {
    (void) params;

    // The rail, the mux and gain lines and the ADC come up here rather than in main() so that a
    // sensor that fails to initialise costs this task and nothing else. A failed init is not
    // fatal: the driver stays callable and reports per-sample faults, which is more informative
    // than never asking.
    const bool sensor_up = Hal::UvInit();
    printf("[sensor] EUV chain %s; sampling every %lu ms\n",
           sensor_up ? "up" : "DOWN (sampling anyway, expect faults)",
           (unsigned long) OWLSAT_SENSOR_PERIOD_MS);

    uint32_t failures = 0;

    // Fixed reference, advanced by exactly one period each loop. A pass takes a variable amount
    // of time — five sequential channel changes, each with its own settling delay, and the
    // auto-range may add more — so pacing with vTaskDelay() would let the cadence drift by
    // however long the array happened to take. Science cadence has to be a property of the
    // schedule, not of the sensor's mood.
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(OWLSAT_SENSOR_PERIOD_MS);

    for (;;) {
      Watchdog::CheckIn(Watchdog::Client::Sensor);

      UvSample sample;
      if (Hal::UvSample(&sample)) {
        failures = 0;

        uint32_t seq = 0;
        if (StorageTable::Append(sample, &seq)) {
          printf("[sensor] seq %lu  E=%.3e W/m2  sigma=%.3e  flags=0x%04X  faces=0x%02X\n",
                 (unsigned long) seq,
                 (double) sample.irradiance_w_m2,
                 (double) sample.sigma_w_m2,
                 (unsigned) sample.flags,
                 (unsigned) sample.face_mask);
        } else {
          // The table refuses an append only when it was never initialised, which main() treats
          // as fatal. Reaching here means a record was acquired and then thrown away.
          printf("[sensor] ERROR: storage table rejected a record\n");
        }
      } else {
        // No fabricated row. A pass that did not happen leaves no trace in the science record
        // beyond this line and the gap in the sequence numbers, which is what the ground needs
        // to see.
        if (failures % FAILURE_LOG_INTERVAL == 0) {
          printf("[sensor] acquisition failed (%lu consecutive)\n",
                 (unsigned long) failures + 1);
        }
        ++failures;
      }

      xTaskDelayUntil(&last_wake, period);
    }
  }

} // namespace OwlSat
