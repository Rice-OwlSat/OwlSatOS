/**
 * @file watchdog_task.cpp
 * @brief Check-in registry and the WDT_WDI kick task.
 */

#include <cstdio>

#include "FreeRTOS.h"
#include "task.h"

#include <OwlSat/config.h>
#include <OwlSat/hal.h>
#include <OwlSat/watchdog.h>

namespace OwlSat::Watchdog {

  namespace {

    constexpr size_t CLIENT_COUNT = static_cast<size_t>(Client::Count);

    /**
     * Last check-in per client, in ticks.
     *
     * Plain volatile words, no mutex. Each entry is a naturally aligned 32-bit store on a
     * Cortex-M33, so a reader never sees a torn value, and the worst a race can do is have the
     * watchdog read the previous check-in and re-check a period later. A lock here would put a
     * blocking call in the one loop that has to keep running when everything else is stuck.
     */
    volatile TickType_t g_last_seen[CLIENT_COUNT];

    /// False takes a client out of the registry — see Suspend().
    volatile bool g_enabled[CLIENT_COUNT];

    /// Deadlines in ticks, indexed by Client. Derived from the task periods in config.h.
    TickType_t g_deadline[CLIENT_COUNT];

    /// Tick at which deadline enforcement begins.
    TickType_t g_grace_until = 0;

    const char *ClientName(size_t index) {
      switch (static_cast<Client>(index)) {
        case Client::Sensor:   return "sensor";
        case Client::Link:     return "link";
        case Client::Transmit: return "transmit";
        case Client::Count:    break;
      }
      return "?";
    }

  } // namespace

  void Init() {
    const TickType_t now = xTaskGetTickCount();

    g_deadline[static_cast<size_t>(Client::Sensor)]   = pdMS_TO_TICKS(OWLSAT_WATCHDOG_DEADLINE_SENSOR_MS);
    g_deadline[static_cast<size_t>(Client::Link)]     = pdMS_TO_TICKS(OWLSAT_WATCHDOG_DEADLINE_LINK_MS);
    g_deadline[static_cast<size_t>(Client::Transmit)] = pdMS_TO_TICKS(OWLSAT_WATCHDOG_DEADLINE_TRANSMIT_MS);

    for (size_t i = 0; i < CLIENT_COUNT; ++i) {
      g_last_seen[i] = now;
      g_enabled[i] = true;
    }

    g_grace_until = now + pdMS_TO_TICKS(OWLSAT_WATCHDOG_BOOT_GRACE_MS);
  }

  void CheckIn(Client client) {
    const size_t index = static_cast<size_t>(client);
    if (index < CLIENT_COUNT) {
      g_last_seen[index] = xTaskGetTickCount();
    }
  }

  void Suspend(Client client) {
    const size_t index = static_cast<size_t>(client);
    if (index < CLIENT_COUNT) {
      g_enabled[index] = false;
      printf("[wdt] %s suspended; no longer gating the pulse train\n", ClientName(index));
    }
  }

  void Resume(Client client) {
    const size_t index = static_cast<size_t>(client);
    if (index < CLIENT_COUNT) {
      // Reset the timestamp first. Enabling a client whose last check-in is ancient would trip
      // the deadline on the very next sweep.
      g_last_seen[index] = xTaskGetTickCount();
      g_enabled[index] = true;
      printf("[wdt] %s resumed\n", ClientName(index));
    }
  }

  void WatchdogTask(void *params) {
    (void) params;

    Hal::WatchdogInit();
    printf("[wdt] kicking every %lu ms once all clients check in (boot grace %lu ms)\n",
           (unsigned long) OWLSAT_WATCHDOG_PERIOD_MS,
           (unsigned long) OWLSAT_WATCHDOG_BOOT_GRACE_MS);

    // Tracks whether the pulse train is currently running, so a stall and a recovery each log
    // once instead of every period.
    bool kicking = true;

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(OWLSAT_WATCHDOG_PERIOD_MS);

    for (;;) {
      const TickType_t now = xTaskGetTickCount();

      // During boot grace, kick unconditionally. The tasks below have not necessarily reached
      // their first check-in yet, and resetting the board for that would make the spacecraft
      // unable to finish booting.
      //
      // Tick subtraction, never comparison: TickType_t wraps after ~49 days at 1 kHz, and
      // `now > deadline` is wrong across that wrap while `now - last >= deadline` stays correct
      // in modular arithmetic. A watchdog that starts resetting the spacecraft seven weeks into
      // the mission is a specific and well-documented way to lose one.
      const bool in_grace = (now - g_grace_until) > (TickType_t) 0x80000000u;

      size_t     late_client = CLIENT_COUNT;
      TickType_t late_by = 0;

      if (!in_grace) {
        for (size_t i = 0; i < CLIENT_COUNT; ++i) {
          if (!g_enabled[i]) {
            continue;
          }
          const TickType_t elapsed = now - g_last_seen[i];
          if (elapsed >= g_deadline[i]) {
            late_client = i;
            late_by = elapsed - g_deadline[i];
            break;
          }
        }
      }

      if (late_client == CLIENT_COUNT) {
        // Everyone is inside their deadline. This pulse is the only claim the flight computer
        // makes to the outside world that it is still alive, and it is worth exactly as much as
        // the check-ins that gated it.
        Hal::WatchdogPulse();

        if (!kicking) {
          printf("[wdt] all clients recovered; pulse train resumed\n");
          kicking = true;
        }
      } else if (kicking) {
        // Stop pulsing and say why. The external part now runs out its own timeout and resets
        // the board unless the client recovers first, which is the intended behaviour: this task
        // does not reset anything itself, it only stops vouching for the system.
        printf("[wdt] STALL: %s last checked in %lu ms ago (%lu ms past deadline);"
               " pulses stopped, external reset pending\n",
               ClientName(late_client),
               (unsigned long) pdTICKS_TO_MS(now - g_last_seen[late_client]),
               (unsigned long) pdTICKS_TO_MS(late_by));
        kicking = false;
      }

      xTaskDelayUntil(&last_wake, period);
    }
  }

} // namespace OwlSat::Watchdog
