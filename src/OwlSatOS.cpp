/**
                         ┌────────┐                         
                 ┌───────┘        └───────┐                 
          ┌┬─────┘                        └─────┬┐          
          │└───┐   /\                  /\   ┌───┘│          
          │    └──/  \                /  \──┘    │          
          │      /    ────┐      ┌────    \      │          
          │     /         └──────┘         \     │          
          │     └───┐                  ┌───┘     │          
          │         └┬────┐      ┌────┬┘         │          
          │          │    └┬─┬┬─┬┘    │          │          
          │          └─────┘ ││ └─────┘          │          
          │                  ││                  │          
          │                  ││                  │          
          │                  ││                  │          
          │                  ││                  │          
          └──┐               ││               ┌──┘          
             └─────┐         ││         ┌─────┘             
                   └─────┐   ││   ┌─────┘                   
      ┌──────┐           └───┴┴───┘           ┌──────┐      
      │ ┌───┐└──────────┐          ┌──────────┘──┬───│      
      │ │   │   │     │ └──────────┘  ┌─────┐    │   │      
      │ │   │   │     │  │     ┌───   │     │    │   │      
      │ │   │   │     │  │     │      ├─────┤    │   │      
      │ └───┘   │  │  │  │     └──┐   │     │    │   │      
      └──────┐  └──┴──┘  │        │   │     │ ┌──────┘      
             └──────────┐└───  ───┘┌──────────┘             
                        └──────────┘                        
-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
 * @file OwlSatOS.cpp
 * @brief Entry point and task creation for OwlSatOS.
 *
 * Brings up stdio and GPIO, initialises the storage table, the watchdog registry and the shared
 * event group, creates the flight tasks, then hands control to the FreeRTOS scheduler.
 *
 * @par The task set
 *   - @c blink     — onboard-LED heartbeat, the cheapest sign of life on the bench
 *   - @c sensor    — acquires the EUV array and writes the storage table  (sensor_task.cpp)
 *   - @c link      — asks the radio whether it will accept frames        (link_task.cpp)
 *   - @c tx        — packs pending records into frames and sends them    (transmit_task.cpp)
 *   - @c wdt       — pulses WDT_WDI while the three above keep checking in (watchdog_task.cpp)
 *
 * The hardware drivers these tasks depend on live on other branches and are stubbed out in
 * hal_stub.cpp, so this build runs the full task graph against absent hardware: it samples
 * nothing, stores nothing durably, and transmits nothing, and says so on the console.
 *
 * @date 29.08.2026
 */

#include <cstdio>

#include "FreeRTOS.h"
#include "task.h"
#include <pico/stdlib.h>

#include <OwlSat/config.h>
#include <OwlSat/hal.h>
#include <OwlSat/storage_table.h>
#include <OwlSat/tasks.h>
#include <OwlSat/watchdog.h>

#define GPIO_ON  1 ///< Logic high — drive GPIO pin high.
#define GPIO_OFF 0 ///< Logic low  — drive GPIO pin low.

constexpr uint LED_PIN = PICO_DEFAULT_LED_PIN;

/**
 * @brief Onboard-LED heartbeat.
 *
 * Deliberately not a watchdog client: it proves the scheduler is running but not that any
 * subsystem is making progress, and gating the external watchdog on a blink would let a
 * spacecraft that has stopped doing science keep insisting it is healthy.
 *
 * @param param Unused (pass nullptr).
 */
void blink(void *param) {
  (void) param;
  for (;;) {
    gpio_put(LED_PIN, GPIO_ON);
    vTaskDelay(pdMS_TO_TICKS(OWLSAT_BLINK_HALF_PERIOD_MS));
    gpio_put(LED_PIN, GPIO_OFF);
    vTaskDelay(pdMS_TO_TICKS(OWLSAT_BLINK_HALF_PERIOD_MS));
  }
}

namespace {

  /**
   * @brief Creates one task and reports failure by name.
   * @return False if the kernel could not allocate the task.
   */
  bool Spawn(TaskFunction_t entry, const char *name, uint16_t stack, UBaseType_t priority) {
    if (xTaskCreate(entry, name, stack, nullptr, priority, nullptr) != pdPASS) {
      printf("FATAL: could not create task '%s' (stack %u words) — heap exhausted?\n",
             name, (unsigned) stack);
      return false;
    }
    return true;
  }

  /**
   * @brief Last resort when the system cannot be brought up.
   *
   * Halts with interrupts disabled and, critically, without ever having pulsed WDT_WDI. A board
   * that cannot finish booting must not sit here quietly: the external watchdog times out and
   * resets it, which is the only recovery path available to a spacecraft with nobody aboard.
   */
  [[noreturn]] void HaltForWatchdog(const char *reason) {
    printf("FATAL: %s — halting; external watchdog will reset the board\n", reason);
    fflush(stdout);
    portDISABLE_INTERRUPTS();
    for (;;) {
    }
  }

} // namespace

/**
 * @brief Firmware entry point.
 *
 * Initialises stdio and GPIO, brings up the OS-level services, creates the flight tasks and
 * starts the scheduler. Never returns.
 *
 * @return int Never reached.
 */
int main() {
  stdio_init_all();
  sleep_ms(300);  // let the UART settle before first output

  // Boot banner: if this prints, main() runs and stdio works (independent of FreeRTOS).
  printf("\n=== OwlSatOS boot: main() reached, stdio up ===\n");

  gpio_init(LED_PIN);
  gpio_set_dir(LED_PIN, GPIO_OUT);

  // OS services, before any task exists. Each of these is something a task will use without
  // checking, so a failure here is a failure to boot and not something to carry on past.
  if (!OwlSat::StorageTable::Init()) {
    HaltForWatchdog("storage table would not initialise");
  }
  if (!OwlSat::InitTaskEvents()) {
    HaltForWatchdog("task event group would not allocate");
  }
  OwlSat::Watchdog::Init();

  // The nonvolatile store is probed here rather than inside a task because nothing owns it: the
  // storage table mirrors to it and the console will eventually read from it. A failure is not
  // fatal — the RAM table carries the mission until the storage branch merges.
  (void) OwlSat::Hal::StorageInit();

  bool ok = true;
  ok = Spawn(blink, "blink", OWLSAT_STACK_BLINK, OWLSAT_PRIO_BLINK) && ok;
  ok = Spawn(OwlSat::SensorTask, "sensor", OWLSAT_STACK_SENSOR, OWLSAT_PRIO_SENSOR) && ok;
  ok = Spawn(OwlSat::LinkTask, "link", OWLSAT_STACK_LINK, OWLSAT_PRIO_LINK) && ok;
  ok = Spawn(OwlSat::TransmitTask, "tx", OWLSAT_STACK_TRANSMIT, OWLSAT_PRIO_TRANSMIT) && ok;
  ok = Spawn(OwlSat::Watchdog::WatchdogTask, "wdt", OWLSAT_STACK_WATCHDOG, OWLSAT_PRIO_WATCHDOG)
       && ok;

  if (!ok) {
    HaltForWatchdog("one or more flight tasks could not be created");
  }

  printf("all tasks created; starting scheduler...\n");
  vTaskStartScheduler();

  // Only reachable if the scheduler could not start, which on this port means the heap could not
  // supply the idle or timer task.
  HaltForWatchdog("scheduler returned");
}

// ---------------------------------------------------------------------------
// FreeRTOS hooks
//
// Both of these are called from a context where the kernel's own invariants are already broken,
// so neither tries to recover. They record what happened and stop, which stops the watchdog task
// with everything else and lets the external circuit do the only useful thing left.
// ---------------------------------------------------------------------------

extern "C" void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
  (void) task;
  printf("\nFATAL: stack overflow in task '%s'\n", name != nullptr ? name : "?");
  fflush(stdout);
  portDISABLE_INTERRUPTS();
  for (;;) {
  }
}

extern "C" void vApplicationMallocFailedHook() {
  printf("\nFATAL: pvPortMalloc failed — FreeRTOS heap exhausted\n");
  fflush(stdout);
  portDISABLE_INTERRUPTS();
  for (;;) {
  }
}
