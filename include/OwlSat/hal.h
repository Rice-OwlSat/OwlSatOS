/**

  @file       hal.h
  @brief      Hardware facade the OwlSatOS tasks are written against.
  @details    The four flight tasks on this branch talk to hardware only through this header.
              The peripheral drivers themselves live on other branches — the EUV chain on
              `sxuv5-interface`, the nonvolatile store on `storage` / `nonvolatile-data-storage`,
              the LTM-1 link on `radio` — and none of them are merged yet.

              @par Why a facade rather than including the real headers
              The task layer and the driver layer are being written in parallel and will meet at
              merge time. Naming the handful of calls the tasks actually need, in one file, means
              the merge is a rewrite of hal_stub.cpp against the real drivers and nothing above it
              moves. It also keeps the task logic testable on the bench with no board attached.

              Every function here is implemented in src/hal_stub.cpp as a no-op that reports
              failure or absence. That is deliberate: a stub that returned plausible data would
              make the tasks look like they work, and the first honest thing this OS should do
              is refuse to invent science.

              @par Threading
              These are called from task context only, never from an ISR. None of them are
              internally serialised. I2C0 is shared board-wide — the EUV ADC sits on it with the
              power monitors, the IMU and the magnetometer — so arbitration belongs to whatever
              owns the bus, not to individual callers.

  @author     Viola Case
  @date       29.08.2026
  @copyright  © Viola Case, 2026. All rights reserved.

**/
#pragma once

#include <cstddef>
#include <cstdint>

#include <OwlSat/telemetry.h>

namespace OwlSat::Hal {

  /// Why the link is or is not currently usable. Reported by RadioQueryReady().
  enum class LinkState : uint8_t {
    Unknown  = 0, ///< Radio has not been asked yet, or the query itself failed.
    Down     = 1, ///< Radio unpowered or not answering.
    NotReady = 2, ///< Powered and answering, but will not accept a frame right now.
    Ready    = 3, ///< Will accept at least one frame.
  };

  /// Snapshot of link readiness, as returned by RadioQueryReady().
  struct LinkStatus {
    LinkState state;
    /// Frames the radio says it can take before it pushes back. Zero unless state == Ready.
    uint16_t  frames_free;
    /// Tick count at which this snapshot was taken.
    uint32_t  uptime_ms;
  };


  // -----------------------------------------------------------------------
  // EUV sensor array  (branch: sxuv5-interface)
  // -----------------------------------------------------------------------

  /**
   * @brief Brings up the science chain: SENS_PWR, the mux and gain lines, I2C0 and the ADC.
   * @return False if the chain did not come up. Sampling stays callable and reports faults.
   */
  bool UvInit();

  /**
   * @brief Acquires one pass across the five faces and reconstructs the irradiance.
   *
   * Maps onto SampleEUV() followed by ScaleEUV() on the sensor branch. Blocks for the pass —
   * five channel changes and their settling delays — so it is called from a task, on a cadence
   * the task owns, never from a timer callback.
   *
   * @param out Filled on success; untouched on failure.
   * @return False if no usable pass was obtained. A pass in which individual faces faulted is
   *         still a success: the failed faces carry their flags and the others carry data.
   */
  bool UvSample(UvSample *out);


  // -----------------------------------------------------------------------
  // Nonvolatile store  (branches: storage, nonvolatile-data-storage)
  // -----------------------------------------------------------------------

  /**
   * @brief Mounts the nonvolatile store and opens the active record file.
   * @return False if the store is unavailable. The storage table still runs, in RAM only.
   */
  bool StorageInit();

  /**
   * @brief Appends one record to the active file on the nonvolatile store.
   *
   * The in-RAM storage table is the system of record while this returns false, which it always
   * does on this branch. Records therefore survive only as long as power does — the loss window
   * is OWLSAT_TABLE_CAPACITY records wide and closes when the storage branch merges.
   *
   * @return False if the record was not durably written.
   */
  bool StorageAppend(const TelemetryRecord &record);

  /// @return True once a nonvolatile store is mounted and accepting appends.
  bool StorageAvailable();


  // -----------------------------------------------------------------------
  // Radio  (branch: radio — AMSAT LTM-1 over CAN, bridged from SPI0)
  // -----------------------------------------------------------------------

  /**
   * @brief Powers the radio domain (RADIO_PWR) and brings up the SPI-to-CAN bridge.
   * @return False if the radio did not come up.
   */
  bool RadioInit();

  /**
   * @brief Asks the radio whether it will accept telemetry frames right now.
   *
   * This is a question for the radio, not a guess from a schedule: the LTM-1 is behind a power
   * switch and a CAN bridge, and neither the pass geometry nor the power budget is knowable
   * from this side of the link.
   *
   * @param out Filled on every call, including failures — a failed query reports
   *            LinkState::Unknown rather than leaving the caller's snapshot stale.
   * @return False if the radio could not be reached.
   */
  bool RadioQueryReady(LinkStatus *out);

  /**
   * @brief Hands one complete frame to the radio.
   *
   * The frame is already framed and checksummed by the caller. Whatever link-layer wrapper the
   * LTM-1 wants — AX.25, COBS, raw CAN segmentation — is applied below this call.
   *
   * @param frame Frame bytes.
   * @param len   Frame length, at most OWLSAT_FRAME_MAX_BYTES.
   * @return False if the frame was not accepted. The caller must not treat the records in an
   *         unaccepted frame as downlinked.
   */
  bool RadioSendPacket(const uint8_t *frame, size_t len);


  // -----------------------------------------------------------------------
  // External watchdog  (WDT_WDI)
  // -----------------------------------------------------------------------

  /// Configures the WDT_WDI line as an output and drives it to its idle level.
  void WatchdogInit();

  /**
   * @brief Emits one pulse on WDT_WDI.
   *
   * Edge-driven, not level-driven: the external part is looking for a transition, so a stuck
   * high line is as much a fault as a silent one. Called only from the watchdog task, and only
   * when every registered client has checked in.
   */
  void WatchdogPulse();

} // namespace OwlSat::Hal
