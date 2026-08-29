/**
 * @file hal_stub.cpp
 * @brief No-op implementation of the OwlSatOS hardware facade.
 *
 * Placeholder for the peripheral drivers being written on the `sxuv5-interface`, `storage` /
 * `nonvolatile-data-storage` and `radio` branches. Every call here reports absence or failure.
 *
 * @par Why these do not return fake data
 * A stub that handed back a plausible irradiance, or claimed the radio was ready, would make the
 * task layer look correct while proving nothing about it. Reporting honest failure exercises the
 * paths that actually matter before launch — a sensor that will not answer, a link that will not
 * take a frame — and makes the absence of hardware visible on the console instead of silently
 * filling the storage table with fiction.
 *
 * @par Merge notes
 * Each function below names the branch and the call that replaces it. Nothing above this file
 * needs to change when they land; this file does.
 */

#include <cstdio>

#include "FreeRTOS.h"
#include "task.h"

#include <OwlSat/hal.h>

namespace OwlSat::Hal {

  namespace {
    /// Set once StorageInit() has run, so StorageAvailable() reports a state and not a guess.
    bool g_storage_probed = false;
  } // namespace


  // -----------------------------------------------------------------------
  // EUV sensor array
  // -----------------------------------------------------------------------

  bool UvInit() {
    // MERGE: sxuv5-interface -> return OwlSat::InitEUV();
    printf("[hal] UvInit: no EUV driver in this build (branch sxuv5-interface)\n");
    return false;
  }

  bool UvSample(OwlSat::UvSample *out) {
    // MERGE: sxuv5-interface ->
    //   const ArraySample pass = OwlSat::SampleEUV();
    //   const EUVResult   fit  = OwlSat::ScaleEUV(pass);      // or ScaleEUV(pass, sun_body)
    //   then compact pass.face[i].raw and fit into *out.
    (void) out;
    return false;
  }


  // -----------------------------------------------------------------------
  // Nonvolatile store
  // -----------------------------------------------------------------------

  bool StorageInit() {
    // MERGE: storage -> mount the FAT12 volume and open the active day-file.
    g_storage_probed = true;
    printf("[hal] StorageInit: no nonvolatile store in this build (branch storage)\n");
    return false;
  }

  bool StorageAppend(const TelemetryRecord &record) {
    // MERGE: storage -> serialise the record into the open day-file.
    (void) record;
    return false;
  }

  bool StorageAvailable() {
    (void) g_storage_probed;
    return false;
  }


  // -----------------------------------------------------------------------
  // Radio
  // -----------------------------------------------------------------------

  bool RadioInit() {
    // MERGE: radio -> assert RADIO_PWR, bring up SPI0 and the CAN bridge to the LTM-1.
    printf("[hal] RadioInit: no radio driver in this build (branch radio)\n");
    return false;
  }

  bool RadioQueryReady(LinkStatus *out) {
    // MERGE: radio -> query the LTM-1's transmit queue depth over CAN.
    if (out != nullptr) {
      // Unknown, not Down: this build cannot distinguish a radio that is off from one that is
      // absent, and reporting Down would assert something the stub has not established.
      out->state       = LinkState::Unknown;
      out->frames_free = 0;
      out->uptime_ms   = static_cast<uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount()));
    }
    return false;
  }

  bool RadioSendPacket(const uint8_t *frame, size_t len) {
    // MERGE: radio -> segment the frame across CAN and hand it to the LTM-1.
    (void) frame;
    (void) len;
    return false;
  }


  // -----------------------------------------------------------------------
  // External watchdog
  // -----------------------------------------------------------------------

  void WatchdogInit() {
    // MERGE: pin_assignment.h has no WDT_WDI macro yet — the harness drawing has not fixed the
    // pin. Once it does: gpio_init(WDT_WDI_PIN); gpio_set_dir(..., GPIO_OUT); gpio_put(..., 0);
    printf("[hal] WatchdogInit: WDT_WDI pin not assigned; pulses go to the console only\n");
  }

  void WatchdogPulse() {
    // MERGE: drive a pulse wide enough for the external part to latch — the datasheet minimum
    // is a board-level number and is not yet fixed:
    //   gpio_put(WDT_WDI_PIN, 1); busy_wait_us(WDT_WDI_PULSE_US); gpio_put(WDT_WDI_PIN, 0);
    //
    // Silent by design. This runs several times a second, and a printf here would drown the
    // console and put newlib's formatting on the one path that has to stay cheap and reliable.
  }

} // namespace OwlSat::Hal
