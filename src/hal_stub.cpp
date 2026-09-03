/**
 * @file hal_stub.cpp
 * @brief No-op implementation of the OwlSatOS hardware facade.
 *
 * Placeholder for the peripheral drivers being written on the `sxuv5-interface` and `storage` /
 * `nonvolatile-data-storage` branches. Every call here reports absence or failure.
 *
 * The radio section is gone: it lives in src/ltm1_link.cpp now, against the AMSAT LTM-1 CAN
 * interface. What remains stubbed for the radio is the CAN controller one layer below it.
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

#include <pico/stdlib.h>

#include <OwlSat/hal.h>
#include <OwlSat/pin_assignment.h>

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
  //
  // MERGED. RadioInit(), RadioQueryReady() and RadioSendPacket() now live in src/ltm1_link.cpp,
  // implemented against the AMSAT LTM-1 over CAN. What is still stubbed there is one layer
  // lower — the SPI-attached CAN controller in src/can_controller_stub.cpp, whose part has not
  // been selected — so this build still reports an honest LinkState::Down.
  // -----------------------------------------------------------------------


  // -----------------------------------------------------------------------
  // External watchdog
  // -----------------------------------------------------------------------

  void WatchdogInit() {
    // Not a stub. WDT_WDI is a line the flight computer owns directly — there is no external
    // part to drive it through — so it needs no branch to land. The pin number and the pulse
    // width are still placeholders from pin_assignment.h, which is why this says so out loud.
#if OWLSAT_PIN_IS_ASSIGNED(WDT_WDI)
    gpio_init(WDT_WDI);
    gpio_set_dir(WDT_WDI, GPIO_OUT);
    gpio_put(WDT_WDI, 0);
    printf("[hal] WatchdogInit: WDT_WDI on GPIO %d (placeholder pin, %d us pulse)\n",
           WDT_WDI, WDT_WDI_PULSE_US);
#else
    printf("[hal] WatchdogInit: WDT_WDI unassigned; pulses go to the console only\n");
#endif
  }

  void WatchdogPulse() {
    // Silent by design. This runs several times a second, and a printf here would drown the
    // console and put newlib's formatting on the one path that has to stay cheap and reliable.
#if OWLSAT_PIN_IS_ASSIGNED(WDT_WDI)
    gpio_put(WDT_WDI, 1);
    busy_wait_us(WDT_WDI_PULSE_US);
    gpio_put(WDT_WDI, 0);
#endif
    // TBC: WDT_WDI_PULSE_US is a placeholder until the external part is selected. Widen it to
    // that part's datasheet minimum, not to whatever happens to work on the bench.
  }

} // namespace OwlSat::Hal
