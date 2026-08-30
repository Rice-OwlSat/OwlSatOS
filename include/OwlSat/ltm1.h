/**

  @file       ltm1.h
  @brief      AMSAT LTM-1 CAN protocol layer: identifiers, science chunking, health telemetry.
  @details    Pure encoding, no hardware and no FreeRTOS. Everything here is a function of its
              arguments, so it is readable, reviewable and checkable against the ICD without a
              board attached. The bus itself lives behind can_controller.h; the policy that
              decides what to send lives in ltm1_link.cpp.

              @par OwlSat is the host, not the radio
              The single most important thing the ICD settles is role. OwlSat is what the ICD
              calls the "host platform"; the LTM is the radio. The LTM owns the entire RF stack —
              665-byte frames, a 4-byte CRC, Reed-Solomon (255,223) forward error correction and
              the 1200 bps BPSK downlink. None of that is ours to build. Our transmit job is to
              put well-formed CAN messages on a 125 kbit/s bus and let the LTM downlink them.

              This is why telemetry.h's OwlSatFrame is *not* what goes on the wire to the radio.
              It survives as an OwlSat-level container that we chunk across science CAN messages
              and that the ground reassembles — opaque to the LTM the whole way. See
              docs/internal/ltm1_link_design.md.

              @par Two paths out
              Science (ChunkFrameToScience) carries OwlSatFrame bytes the LTM never interprets.
              Health (BuildHealthMessages) maps OwlSat state onto the ICD's own fixed message
              IDs so FoxTelem renders it as temperatures and voltages rather than as a blob.
              Both are compiled in and independently switchable; see OWLSAT_LTM_SEND_* in
              config.h. One of them will likely be dropped once the ground segment settles.

  @warning    Field layouts here are transcribed from AMSAT LTM ICD v2.3 Tables 6, 7 and 8. That
              document is marked AMSAT proprietary — this header carries the encodings firmware
              needs and cites sections, not the document's text.

  @author     Viola Case
  @date       29.08.2026
  @copyright  (c) Viola Case, 2026. All rights reserved.

**/
#pragma once

#include <cstddef>
#include <cstdint>

#include <OwlSat/config.h>
#include <OwlSat/telemetry.h>

namespace OwlSat::Ltm1 {

  // =========================================================================
  // Bus primitive
  // =========================================================================

  /// Largest payload a classic-CAN data frame carries. The LTM bus is classic CAN, not FD.
  constexpr size_t CAN_MAX_DATA = 8;

  /// One CAN data frame, extended (29-bit) identifier. The bus form of everything below.
  struct CanMessage {
    /// 29-bit extended identifier, already packed. See PackId().
    uint32_t id;
    /// Data length code, 0..CAN_MAX_DATA.
    uint8_t  dlc;
    uint8_t  data[CAN_MAX_DATA];
  };


  // =========================================================================
  // Identifier layout  (ICD Table 6)
  // =========================================================================

  //  28:24  priority     5 bits   lower value wins arbitration
  //  23:20  source       4 bits   host uses >= 8; the LTM uses 3
  //  19:16  type         4 bits   MsgType below
  //  15:12  destination  4 bits   host -> LTM must be 3 or 0; LTM -> host is 8
  //  11:0   message id  12 bits   Table 7 for status, Table 8 for health, ours for science

  constexpr unsigned ID_SHIFT_PRIORITY = 24;
  constexpr unsigned ID_SHIFT_SOURCE   = 20;
  constexpr unsigned ID_SHIFT_TYPE     = 16;
  constexpr unsigned ID_SHIFT_DEST     = 12;
  constexpr unsigned ID_SHIFT_MSGID    = 0;

  constexpr uint32_t ID_MASK_PRIORITY = 0x1Fu;
  constexpr uint32_t ID_MASK_SOURCE   = 0x0Fu;
  constexpr uint32_t ID_MASK_TYPE     = 0x0Fu;
  constexpr uint32_t ID_MASK_DEST     = 0x0Fu;
  constexpr uint32_t ID_MASK_MSGID    = 0x0FFFu;

  /**
   * @brief Type field values (ICD Table 6, bits 19:16).
   *
   * The type says which LTM operational mode the data is downlinked in, not what the data is.
   * Adding 8 to the safe- and health-mode realtime types additionally saves the data as Whole
   * Orbit Data, which is how a payload survives being acquired outside a ground station pass.
   */
  enum class MsgType : uint8_t {
    SafeRealtime         = 1,  ///< Opaque, downlinked in realtime while the LTM is in safe mode.
    HealthRealtime       = 2,  ///< Opaque, downlinked in realtime while in health mode (normal).
    ScienceRealtime      = 3,  ///< Opaque, downlinked in science mode only.
    Status               = 4,  ///< Mode coordination, both directions. Table 7.
    Health               = 5,  ///< Rendered by FoxTelem as health data, and saved as health WOD.
    WodOnly              = 8,  ///< Saved as Whole Orbit Data, not downlinked in realtime.
    SafeRealtimeAndWod   = 9,  ///< 1 + 8.
    HealthRealtimeAndWod = 10, ///< 2 + 8. OwlSat's default for science — see config.h.
  };

  /// The destination the LTM answers to. Anything else it discards (ICD Table 6).
  constexpr uint8_t DEST_LTM = 3;

  /// The destination the LTM stamps on messages bound for us. The host ignores anything below 8.
  constexpr uint8_t DEST_HOST_MIN = 8;

  /// The source the LTM stamps on its own messages.
  constexpr uint8_t SOURCE_LTM = 3;

  /// Decoded identifier fields, as returned by UnpackId().
  struct CanIdFields {
    uint8_t  priority;
    uint8_t  source;
    MsgType  type;
    uint8_t  dest;
    uint16_t msg_id;
  };

  /**
   * @brief Packs the five Table 6 fields into a 29-bit extended identifier.
   *
   * Every argument is masked to its field width rather than asserted. A caller that passed an
   * out-of-range source would otherwise corrupt the neighbouring field and produce an ID the
   * LTM silently drops, which is far harder to see on a bus than a wrong-but-well-formed ID.
   */
  constexpr uint32_t PackId(uint8_t priority, uint8_t source, MsgType type, uint8_t dest, uint16_t msg_id) {
    return ((static_cast<uint32_t>(priority) & ID_MASK_PRIORITY) << ID_SHIFT_PRIORITY) |
           ((static_cast<uint32_t>(source) & ID_MASK_SOURCE) << ID_SHIFT_SOURCE) |
           ((static_cast<uint32_t>(type) & ID_MASK_TYPE) << ID_SHIFT_TYPE) |
           ((static_cast<uint32_t>(dest) & ID_MASK_DEST) << ID_SHIFT_DEST) |
           ((static_cast<uint32_t>(msg_id) & ID_MASK_MSGID) << ID_SHIFT_MSGID);
  }

  /// @brief Splits a 29-bit identifier back into its Table 6 fields.
  constexpr CanIdFields UnpackId(uint32_t id) {
    return CanIdFields{
        static_cast<uint8_t>((id >> ID_SHIFT_PRIORITY) & ID_MASK_PRIORITY),
        static_cast<uint8_t>((id >> ID_SHIFT_SOURCE) & ID_MASK_SOURCE),
        static_cast<MsgType>((id >> ID_SHIFT_TYPE) & ID_MASK_TYPE),
        static_cast<uint8_t>((id >> ID_SHIFT_DEST) & ID_MASK_DEST),
        static_cast<uint16_t>((id >> ID_SHIFT_MSGID) & ID_MASK_MSGID),
    };
  }

  /**
   * @brief Whether an identifier seen on the bus is addressed to this host.
   *
   * The ICD requires the host to ignore anything with a destination below 8; that range belongs
   * to the LTM and to other hosts. Applied before any inbound message is interpreted.
   */
  constexpr bool IsForHost(uint32_t id) { return UnpackId(id).dest >= DEST_HOST_MIN; }

  /// @brief Builds an identifier for a message this host sends to the LTM.
  constexpr uint32_t HostToLtm(uint8_t priority, MsgType type, uint16_t msg_id) {
    return PackId(priority, OWLSAT_LTM_SOURCE_ID, type, DEST_LTM, msg_id);
  }


  // =========================================================================
  // Status messages  (ICD Table 7, type 4)
  // =========================================================================

  /// Message IDs for MsgType::Status. Sent by either side to coordinate a mode change.
  enum class StatusMsg : uint16_t {
    Ignored          = 0, ///< Explicitly ignored by the receiver. Useful as a bus keepalive.
    EnterSafeMode    = 1, ///< Data0 = reason: 0 commanded, 1 low power.
    EnterHealthMode  = 2, ///< Data0 = reason: 0 commanded, 1 power ok, 2 science timed out.
    EnterScienceMode = 3, ///< Data0 = reason: 0 commanded, 1 scheduled; Data1 = timeout [min].
  };

  /**
   * @brief The LTM's operational mode, as last reported over the bus.
   *
   * Not a mode we choose. The LTM owns it; we learn about changes from inbound Table 7 messages
   * and we may ask for one, which is a request and not a command.
   */
  enum class LtmMode : uint8_t {
    Unknown = 0, ///< Nothing heard from the LTM yet since boot, or the bus is down.
    Safe    = 1, ///< Beacon reduced to ~10 s every 2 minutes; transponder off; health data only.
    Health  = 2, ///< Normal. Continuous telemetry, whole orbit data, and host science.
    Science = 3, ///< Timed. More host science at the expense of health and WOD.
  };

  /// @brief Maps an inbound Table 7 status message ID onto the mode it announces.
  constexpr LtmMode ModeFromStatus(StatusMsg msg) {
    switch (msg) {
      case StatusMsg::EnterSafeMode:    return LtmMode::Safe;
      case StatusMsg::EnterHealthMode:  return LtmMode::Health;
      case StatusMsg::EnterScienceMode: return LtmMode::Science;
      case StatusMsg::Ignored:          break;
    }
    return LtmMode::Unknown;
  }

  /**
   * @brief Whether host science is actually downlinked while the LTM is in @p mode.
   *
   * Science and Science WOD payloads are sent in health mode; long science payloads are sent in
   * science mode (ICD 2.6.5). In safe mode the LTM is shedding load and carries health data
   * only, so science handed over then is buffered at best and dropped at worst.
   */
  constexpr bool ModeCarriesScience(LtmMode mode) {
    return mode == LtmMode::Health || mode == LtmMode::Science;
  }

  /// @brief Builds a status message asking the LTM to change mode. @p reason follows Table 7.
  CanMessage BuildStatusRequest(StatusMsg msg, uint8_t reason, uint8_t detail = 0);


  // =========================================================================
  // Health telemetry  (ICD Table 8, type 5)
  // =========================================================================

  /// Message IDs for MsgType::Health. Each has a fixed meaning and a fixed scale on the ground.
  enum class HealthMsg : uint16_t {
    SolarPanelTemps    = 8,  ///< Data 0..5 = +X, -X, +Y, -Y, +Z, -Z.
    OtherTemps0        = 9,  ///< Up to 8 temperatures, FoxTelem labels Temp 0..7.
    OtherTemps8        = 10, ///< Up to 8 temperatures, FoxTelem labels Temp 8..15.
    SolarPanelVoltages = 16, ///< Data 0..5 = +X, -X, +Y, -Y, +Z, -Z.
    OtherVoltages0     = 17, ///< Up to 8 voltages, FoxTelem labels Voltage 0..7.
    OtherVoltages8     = 18, ///< Up to 8 voltages, FoxTelem labels Voltage 8..15.
    BinaryState1       = 24, ///< Up to 32 state bits in data 0..3.
    BinaryState2       = 25, ///< Up to 24 state bits in data 0..2.
    BinaryState3       = 26, ///< Up to 16 state bits in data 0..1.
    BinaryState4       = 27, ///< Up to 8 state bits in data 0.
    UtcTime            = 32, ///< Data 0..5 = year, month, day, hour, minute, second.
  };

  /// Faces reported by the solar panel temperature and voltage messages, in ICD data order.
  constexpr size_t PANEL_COUNT = 6;

  /// Temperature scale endpoints [degC] for the Table 8 one-byte encoding.
  constexpr float TEMP_MIN_C = -20.0f;
  constexpr float TEMP_MAX_C = 107.5f;

  /// Voltage scale endpoints [V] for the Table 8 one-byte encoding.
  constexpr float VOLT_MIN_V = 0.0f;
  constexpr float VOLT_MAX_V = 10.0f;

  /**
   * @brief Encodes a temperature into the Table 8 byte: 0..255 spans -20 degC to +107.5 degC.
   *
   * Saturates at the endpoints rather than wrapping. A thermistor reading off the bottom of the
   * scale is a cold spacecraft, and reporting it as 107 degC would be a fault report inverted.
   */
  uint8_t EncodeTempC(float celsius);

  /// @brief Encodes a voltage into the Table 8 byte: 0..255 spans 0 V to 10 V. Saturates.
  uint8_t EncodeVolts(float volts);

  /**
   * @brief The subset of OwlSat state that maps onto the ICD's own health telemetry.
   *
   * Every group carries a validity flag because most of these sensors are on branches that have
   * not merged. An absent group is *not emitted* rather than sent as zero: a zero here decodes on
   * the ground as -20 degC or 0 V, which reads as a real and alarming measurement. Silence is the
   * honest encoding of "not fitted yet", and it costs bus bandwidth we would rather not spend.
   */
  struct HealthSnapshot {
    /// Solar panel temperatures [degC], in +X, -X, +Y, -Y, +Z, -Z order.
    float panel_temp_c[PANEL_COUNT];
    bool  panel_temp_valid;

    /// Solar panel voltages [V], same face order.
    float panel_volts[PANEL_COUNT];
    bool  panel_volts_valid;

    /// Housekeeping temperatures [degC] — battery pack, MCU core, radio domain, and spares.
    float temp_c[CAN_MAX_DATA];
    /// Bit i set when temp_c[i] holds a real measurement.
    uint8_t temp_valid_mask;

    /// Housekeeping voltages [V] — battery, 3V3 rail, RFvdd, and spares.
    float volts[CAN_MAX_DATA];
    /// Bit i set when volts[i] holds a real measurement.
    uint8_t volts_valid_mask;

    /// 32 spacecraft state bits, downlinked as BinaryState1. See StateBit below.
    uint32_t state_bits;
    bool     state_valid;

    /// UTC from the GPS. utc_year is the two-digit year the ICD's one-byte field carries.
    uint8_t utc_year, utc_month, utc_day, utc_hour, utc_minute, utc_second;
    bool    utc_valid;
  };

  /// Bit assignments within HealthSnapshot::state_bits, as FoxTelem's State 0..31.
  enum StateBit : uint32_t {
    STATE_ANTENNA_DEPLOYED = 1u << 0, ///< Burn wires fired and the NVM key was written.
    STATE_RADIO_POWERED    = 1u << 1, ///< RADIO_PWR asserted.
    STATE_SENSOR_POWERED   = 1u << 2, ///< SENS_PWR asserted.
    STATE_STORAGE_MOUNTED  = 1u << 3, ///< Nonvolatile store mounted and accepting appends.
    STATE_LINK_READY       = 1u << 4, ///< EVT_LINK_READY was set at snapshot time.
    STATE_UV_HEALTHY       = 1u << 5, ///< Last EUV pass returned usable data.
    STATE_STORAGE_DROPPING = 1u << 6, ///< The storage ring has overwritten undownlinked records.
    STATE_HEATER_ON        = 1u << 7, ///< HEAT asserted.
  };

  /// Upper bound on messages BuildHealthMessages() can emit — one per HealthMsg it populates.
  constexpr size_t HEALTH_MAX_MESSAGES = 7;

  /**
   * @brief Emits the Table 8 messages that @p snap has real data for.
   *
   * @param snap Snapshot to encode. Invalid groups are skipped entirely.
   * @param out  Buffer of at least HEALTH_MAX_MESSAGES messages.
   * @param max  Capacity of @p out.
   * @return Messages written. Zero when nothing in the snapshot is valid, which is the correct
   *         report from a spacecraft whose sensor branches have not merged.
   */
  size_t BuildHealthMessages(const HealthSnapshot &snap, CanMessage *out, size_t max);


  // =========================================================================
  // Science chunking  (ICD 2.5.1 / 2.6.5, opaque payload)
  // =========================================================================

  /**
   * Messages needed for the largest OwlSatFrame this build can emit.
   *
   * The LTM charges 2 bytes of payload budget per CAN message — 12 identifier bits plus a 4-bit
   * length — on top of the data bytes, so full 8-byte messages are the only efficient size and
   * the only one this chunker emits (bar the last).
   */
  constexpr size_t SCIENCE_MAX_MESSAGES = (OWLSAT_FRAME_MAX_BYTES + CAN_MAX_DATA - 1) / CAN_MAX_DATA;

  /**
   * @brief Splits one OwlSatFrame into opaque science CAN messages for the LTM.
   *
   * The 12-bit message ID carries the 0-based chunk index within the frame; the LTM never looks
   * at it, and the ground uses it together with the frame's own sync bytes to reassemble. Two
   * properties fall out of putting the counter there rather than in the data:
   *
   *   - Every data byte is frame payload, so a chunk costs nothing but the LTM's fixed 2 bytes.
   *   - Lower chunk index is a numerically lower identifier, and CAN arbitration favours lower
   *     identifiers, so chunks of one frame naturally win the bus in order.
   *
   * A frame spans more than one of the LTM's 83-byte science payloads (see the design doc); that
   * is the LTM's business, not ours, and the chunk indices stay continuous across the boundary.
   *
   * @param frame Frame bytes, as produced by BuildTelemetryFrame().
   * @param len   Frame length, at most OWLSAT_FRAME_MAX_BYTES.
   * @param out   Buffer of at least SCIENCE_MAX_MESSAGES messages.
   * @param max   Capacity of @p out.
   * @return Messages written, or 0 if @p len is zero or @p out cannot hold the whole frame. A
   *         partial frame is never emitted — half a frame on the bus is bandwidth spent on
   *         something the ground discards on the CRC anyway.
   */
  size_t ChunkFrameToScience(const uint8_t *frame, size_t len, CanMessage *out, size_t max);

} // namespace OwlSat::Ltm1
