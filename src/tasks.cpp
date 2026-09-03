/**
 * @file tasks.cpp
 * @brief Shared state between the OwlSatOS flight tasks.
 */

#include "FreeRTOS.h"
#include "event_groups.h"

#include <OwlSat/tasks.h>

namespace OwlSat {

  namespace {
    EventGroupHandle_t g_events = nullptr;
  } // namespace

  bool InitTaskEvents() {
    if (g_events == nullptr) {
      g_events = xEventGroupCreate();
    }
    return g_events != nullptr;
  }

  EventGroupHandle_t TaskEvents() {
    return g_events;
  }

} // namespace OwlSat
