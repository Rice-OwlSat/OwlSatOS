/**
 * @file ltm1_can_id.cpp
 * @brief Encoders for the AMSAT LTM-1 CAN protocol layer declared in ltm1.h.
 *
 * Pure functions over their arguments — no hardware, no FreeRTOS, no statics. The compile-time
 * checks at the bottom of this file are the closest thing this branch has to a test suite, and
 * they are deliberate: the encodings here are checkable against the ICD by inspection, and a
 * wrong one costs a launch, so they are checked where the cost of checking is zero.
 */

#include <cstring>

#include <OwlSat/ltm1.h>

namespace OwlSat::Ltm1 {

  namespace {

    /// Clamps @p value into [lo, hi] before it is scaled into a byte.
    constexpr float Clamp(float value, float lo, float hi) {
      if (value < lo) return lo;
      if (value > hi) return hi;
      return value;
    }

    /**
     * Scales @p value across [lo, hi] onto 0..255.
     *
     * Rounds to nearest rather than truncating. Truncation would bias every temperature in the
     * downlink half a step cold, which is small, systematic, and exactly the kind of error that
     * survives review because each individual reading looks plausible.
     */
    uint8_t ScaleToByte(float value, float lo, float hi) {
      const float clamped = Clamp(value, lo, hi);
      const float scaled  = (clamped - lo) * 255.0f / (hi - lo);
      return static_cast<uint8_t>(scaled + 0.5f);
    }

    /// Starts a message addressed to the LTM, with @p dlc data bytes zeroed.
    CanMessage BeginMessage(uint8_t priority, MsgType type, uint16_t msg_id, uint8_t dlc) {
      CanMessage msg = {};
      msg.id         = HostToLtm(priority, type, msg_id);
      msg.dlc        = dlc;
      return msg;
    }

  } // namespace


  // -----------------------------------------------------------------------
  // Scaling  (ICD Table 8)
  // -----------------------------------------------------------------------

  uint8_t EncodeTempC(float celsius) { return ScaleToByte(celsius, TEMP_MIN_C, TEMP_MAX_C); }

  uint8_t EncodeVolts(float volts) { return ScaleToByte(volts, VOLT_MIN_V, VOLT_MAX_V); }


  // -----------------------------------------------------------------------
  // Status  (ICD Table 7, type 4)
  // -----------------------------------------------------------------------

  CanMessage BuildStatusRequest(StatusMsg msg, uint8_t reason, uint8_t detail) {
    // Two data bytes always, even for the messages Table 7 only defines one for. The LTM reads
    // the fields it knows about and a spare zero costs a byte on a bus that is not the
    // bottleneck; a variable DLC here would mean a second thing to get wrong per message type.
    CanMessage out = BeginMessage(OWLSAT_LTM_PRIORITY_STATUS, MsgType::Status,
                                  static_cast<uint16_t>(msg), 2);
    out.data[0]    = reason;
    out.data[1]    = detail;
    return out;
  }


  // -----------------------------------------------------------------------
  // Health  (ICD Table 8, type 5)
  // -----------------------------------------------------------------------

  size_t BuildHealthMessages(const HealthSnapshot &snap, CanMessage *out, size_t max) {
    if (out == nullptr) {
      return 0;
    }

    size_t n = 0;

    /// Appends a message if there is room. Returns false once the buffer is full.
    const auto push = [&](const CanMessage &msg) -> bool {
      if (n >= max) {
        return false;
      }
      out[n++] = msg;
      return true;
    };

    if (snap.panel_temp_valid) {
      CanMessage msg = BeginMessage(OWLSAT_LTM_PRIORITY_HEALTH, MsgType::Health,
                                    static_cast<uint16_t>(HealthMsg::SolarPanelTemps), PANEL_COUNT);
      for (size_t i = 0; i < PANEL_COUNT; ++i) {
        msg.data[i] = EncodeTempC(snap.panel_temp_c[i]);
      }
      if (!push(msg)) return n;
    }

    if (snap.panel_volts_valid) {
      CanMessage msg = BeginMessage(OWLSAT_LTM_PRIORITY_HEALTH, MsgType::Health,
                                    static_cast<uint16_t>(HealthMsg::SolarPanelVoltages), PANEL_COUNT);
      for (size_t i = 0; i < PANEL_COUNT; ++i) {
        msg.data[i] = EncodeVolts(snap.panel_volts[i]);
      }
      if (!push(msg)) return n;
    }

    // The housekeeping groups are all-or-nothing per message: Table 8 gives no per-slot validity,
    // so a partially populated message would silently report -20 degC for every unfitted sensor.
    // The DLC is trimmed to the highest valid slot instead, which reports fewer values rather
    // than wrong ones. Gaps below that slot are the one case we cannot express and are sent as
    // the bottom of scale — recorded in the design doc as a known limitation of Table 8.
    if (snap.temp_valid_mask != 0) {
      uint8_t count = 0;
      for (uint8_t i = 0; i < CAN_MAX_DATA; ++i) {
        if ((snap.temp_valid_mask >> i) & 1u) count = static_cast<uint8_t>(i + 1);
      }
      CanMessage msg = BeginMessage(OWLSAT_LTM_PRIORITY_HEALTH, MsgType::Health,
                                    static_cast<uint16_t>(HealthMsg::OtherTemps0), count);
      for (uint8_t i = 0; i < count; ++i) {
        msg.data[i] = EncodeTempC(snap.temp_c[i]);
      }
      if (!push(msg)) return n;
    }

    if (snap.volts_valid_mask != 0) {
      uint8_t count = 0;
      for (uint8_t i = 0; i < CAN_MAX_DATA; ++i) {
        if ((snap.volts_valid_mask >> i) & 1u) count = static_cast<uint8_t>(i + 1);
      }
      CanMessage msg = BeginMessage(OWLSAT_LTM_PRIORITY_HEALTH, MsgType::Health,
                                    static_cast<uint16_t>(HealthMsg::OtherVoltages0), count);
      for (uint8_t i = 0; i < count; ++i) {
        msg.data[i] = EncodeVolts(snap.volts[i]);
      }
      if (!push(msg)) return n;
    }

    if (snap.state_valid) {
      // All 32 bits in BinaryState1. Table 8's footnote allows splitting them across messages 24
      // through 27 in multiples of 8, but only under the constraint BSD1 >= BSD2 >= BSD3 >= BSD4;
      // one full message satisfies that trivially and leaves the split available if the ground
      // ever wants the bits grouped by subsystem.
      CanMessage msg = BeginMessage(OWLSAT_LTM_PRIORITY_HEALTH, MsgType::Health,
                                    static_cast<uint16_t>(HealthMsg::BinaryState1), 4);
      msg.data[0]    = static_cast<uint8_t>(snap.state_bits & 0xFFu);
      msg.data[1]    = static_cast<uint8_t>((snap.state_bits >> 8) & 0xFFu);
      msg.data[2]    = static_cast<uint8_t>((snap.state_bits >> 16) & 0xFFu);
      msg.data[3]    = static_cast<uint8_t>((snap.state_bits >> 24) & 0xFFu);
      if (!push(msg)) return n;
    }

    if (snap.utc_valid) {
      CanMessage msg = BeginMessage(OWLSAT_LTM_PRIORITY_HEALTH, MsgType::Health,
                                    static_cast<uint16_t>(HealthMsg::UtcTime), 6);
      msg.data[0]    = snap.utc_year;
      msg.data[1]    = snap.utc_month;
      msg.data[2]    = snap.utc_day;
      msg.data[3]    = snap.utc_hour;
      msg.data[4]    = snap.utc_minute;
      msg.data[5]    = snap.utc_second;
      if (!push(msg)) return n;
    }

    return n;
  }


  // -----------------------------------------------------------------------
  // Science chunking
  // -----------------------------------------------------------------------

  size_t ChunkFrameToScience(const uint8_t *frame, size_t len, CanMessage *out, size_t max) {
    if (frame == nullptr || out == nullptr || len == 0) {
      return 0;
    }

    const size_t needed = (len + CAN_MAX_DATA - 1) / CAN_MAX_DATA;

    // All of the frame or none of it. A caller with a short buffer gets a refusal it can act on,
    // rather than a prefix that looks like success and reaches the ground as a CRC failure.
    if (needed > max || needed > ID_MASK_MSGID + 1) {
      return 0;
    }

    for (size_t chunk = 0; chunk < needed; ++chunk) {
      const size_t offset = chunk * CAN_MAX_DATA;
      const size_t remain = len - offset;
      const size_t take   = remain < CAN_MAX_DATA ? remain : CAN_MAX_DATA;

      CanMessage &msg = out[chunk];
      msg             = CanMessage{};
      msg.id  = HostToLtm(OWLSAT_LTM_PRIORITY_SCIENCE, static_cast<MsgType>(OWLSAT_LTM_SCIENCE_TYPE),
                          static_cast<uint16_t>(chunk));
      msg.dlc = static_cast<uint8_t>(take);
      std::memcpy(msg.data, frame + offset, take);
    }

    return needed;
  }


  // =========================================================================
  // Compile-time checks against the ICD
  // =========================================================================

  namespace {

    // --- Identifier layout (Table 6) ---

    // The five fields occupy exactly 29 bits and do not overlap.
    static_assert((ID_MASK_PRIORITY << ID_SHIFT_PRIORITY) == 0x1F000000u, "priority field moved");
    static_assert((ID_MASK_SOURCE << ID_SHIFT_SOURCE) == 0x00F00000u, "source field moved");
    static_assert((ID_MASK_TYPE << ID_SHIFT_TYPE) == 0x000F0000u, "type field moved");
    static_assert((ID_MASK_DEST << ID_SHIFT_DEST) == 0x0000F000u, "destination field moved");
    static_assert((ID_MASK_MSGID << ID_SHIFT_MSGID) == 0x00000FFFu, "message id field moved");
    static_assert(((ID_MASK_PRIORITY << ID_SHIFT_PRIORITY) | (ID_MASK_SOURCE << ID_SHIFT_SOURCE) |
                   (ID_MASK_TYPE << ID_SHIFT_TYPE) | (ID_MASK_DEST << ID_SHIFT_DEST) |
                   (ID_MASK_MSGID << ID_SHIFT_MSGID)) == 0x1FFFFFFFu,
                  "Table 6 fields do not tile a 29-bit identifier");

    // Pack and unpack round-trip, with every field distinguishable from its neighbours.
    constexpr uint32_t    kProbe       = PackId(0x11, 0x9, MsgType::Health, 0x3, 0x5A5);
    constexpr CanIdFields kProbeFields = UnpackId(kProbe);
    static_assert(kProbeFields.priority == 0x11, "priority does not round-trip");
    static_assert(kProbeFields.source == 0x9, "source does not round-trip");
    static_assert(kProbeFields.type == MsgType::Health, "type does not round-trip");
    static_assert(kProbeFields.dest == 0x3, "destination does not round-trip");
    static_assert(kProbeFields.msg_id == 0x5A5, "message id does not round-trip");
    static_assert(kProbe <= 0x1FFFFFFFu, "identifier does not fit 29 bits");

    // Host-side addressing rules the ICD states as requirements, not preferences.
    static_assert(OWLSAT_LTM_SOURCE_ID >= 8u,
                  "ICD Table 6: host source must be 8 or above when addressing the LTM");
    static_assert(OWLSAT_LTM_SOURCE_ID <= ID_MASK_SOURCE, "host source does not fit 4 bits");
    static_assert(DEST_LTM == 3 || DEST_LTM == 0, "ICD Table 6: the LTM only accepts dest 3 or 0");
    static_assert(UnpackId(HostToLtm(0, MsgType::Status, 0)).dest == DEST_LTM,
                  "outbound messages are not addressed to the LTM");
    static_assert(!IsForHost(HostToLtm(0, MsgType::Status, 0)),
                  "our own outbound traffic would be mistaken for inbound");
    static_assert(IsForHost(PackId(0, SOURCE_LTM, MsgType::Status, DEST_HOST_MIN, 0)),
                  "a message the LTM addresses to us is not recognised as inbound");

    // Priorities order our own traffic the way the comments claim they do.
    static_assert(OWLSAT_LTM_PRIORITY_STATUS < OWLSAT_LTM_PRIORITY_HEALTH,
                  "mode coordination must outrank health telemetry");
    static_assert(OWLSAT_LTM_PRIORITY_HEALTH < OWLSAT_LTM_PRIORITY_SCIENCE,
                  "health telemetry must outrank bulk science");
    static_assert(OWLSAT_LTM_PRIORITY_SCIENCE <= ID_MASK_PRIORITY, "priority does not fit 5 bits");

    // --- Science chunking ---

    // Chunk indices live in the 12-bit message id, so a frame can never need more chunks than
    // that field can count.
    static_assert(SCIENCE_MAX_MESSAGES <= ID_MASK_MSGID + 1,
                  "a maximum frame needs more chunks than the message id field can number");

    // Lower chunk index must be a numerically lower identifier for the in-order arbitration
    // property in ltm1.h to hold. True only while the chunk counter is the identifier's low bits.
    static_assert(HostToLtm(OWLSAT_LTM_PRIORITY_SCIENCE, MsgType::HealthRealtimeAndWod, 0) <
                      HostToLtm(OWLSAT_LTM_PRIORITY_SCIENCE, MsgType::HealthRealtimeAndWod, 1),
                  "chunk order does not follow identifier order");

    // The configured science type must be one the ICD actually defines, or the LTM drops it.
    static_assert(OWLSAT_LTM_SCIENCE_TYPE == 1u || OWLSAT_LTM_SCIENCE_TYPE == 2u ||
                      OWLSAT_LTM_SCIENCE_TYPE == 3u || OWLSAT_LTM_SCIENCE_TYPE == 8u ||
                      OWLSAT_LTM_SCIENCE_TYPE == 9u || OWLSAT_LTM_SCIENCE_TYPE == 10u,
                  "OWLSAT_LTM_SCIENCE_TYPE is not a telemetry type from ICD Table 6");

    // --- Health ---

    // Every group BuildHealthMessages() can emit has somewhere to go in the worst case.
    static_assert(HEALTH_MAX_MESSAGES >= 6,
                  "HEALTH_MAX_MESSAGES cannot hold every group BuildHealthMessages emits");
    static_assert(PANEL_COUNT <= CAN_MAX_DATA, "six panel faces must fit one CAN message");

    // Table 8 scale endpoints. -20 degC and +107.5 degC across 256 codes is exactly 0.5 degC per
    // code; if that stops being true the ground decodes every temperature wrongly.
    static_assert(TEMP_MAX_C - TEMP_MIN_C == 127.5f, "Table 8 temperature span is not 127.5 degC");
    static_assert(VOLT_MAX_V - VOLT_MIN_V == 10.0f, "Table 8 voltage span is not 10 V");

  } // namespace

} // namespace OwlSat::Ltm1
