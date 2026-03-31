#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

// --- Scheduler behaviour ---
#define configUSE_PREEMPTION                    1   // Scheduler can interrupt tasks
#define configUSE_TICKLESS_IDLE                 0   // Power saving. Off for now.
#define configUSE_IDLE_HOOK                     0   // Custom function called when CPU is idle. Off for now.
#define configUSE_TICK_HOOK                     0   // Custom function called every tick. Off for now.

// --- Clock & timing ---
#define configCPU_CLOCK_HZ                      133000000   // RP2350 default clock: 133MHz
#define configTICK_RATE_HZ                      1000        // 1000 ticks/sec = 1ms resolution
                                                            // vTaskDelay(1000) = 1 second

// --- Task settings ---
#define configMAX_PRIORITIES                    5    // 0 (lowest) to 4 (highest).
#define configMINIMAL_STACK_SIZE                256  // Stack for the idle task, in words (not bytes)
#define configTOTAL_HEAP_SIZE                   (128 * 1024)  // 128KB. RP2350 has 520KB so we're comfortable.

// --- Task name length ---
#define configMAX_TASK_NAME_LEN                 16

// --- Misc ---
#define configUSE_16_BIT_TICKS                  0   // Use 32-bit tick counter. Always 0 on 32-bit hardware.
#define configIDLE_SHOULD_YIELD                 1   // Idle task yields to other same-priority tasks.

// --- API feature switches ---
// These enable/disable parts of the FreeRTOS API.
// Setting to 1 includes that code in your binary.
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskSuspend                    1

// --- Assertion handler ---
#define configASSERT(x) if((x) == 0) { portDISABLE_INTERRUPTS(); for(;;); }
// If something goes catastrophically wrong inside FreeRTOS, freeze.
// Better than silent corruption. You'll know something broke.

// --- RP2350 Cortex-M33 specific ---
#define configENABLE_FPU                        1   // RP2350 has a hardware FPU, use it
#define configENABLE_MPU                        0   // Memory Protection Unit - off for now
#define configENABLE_TRUSTZONE                  0   // TrustZone security - off for now

#define configMAX_SYSCALL_INTERRUPT_PRIORITY    16  // Interrupts at this priority and below
                                                    // can safely call FreeRTOS API functions

                                                    #define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               3
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            256

#define INCLUDE_xEventGroupSetBitsFromISR       1
#define configUSE_EVENT_GROUPS                  1

#define INCLUDE_xTimerPendFunctionCall          1
                                                    

#endif