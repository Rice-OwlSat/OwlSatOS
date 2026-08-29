/**
 * @file telemetry.cpp
 * @brief CRC and wire serialisation for the OwlSatOS downlink frame.
 */

#include <cstring>

#include "FreeRTOS.h"  // configASSERT

#include <OwlSat/telemetry.h>

namespace OwlSat {

  namespace {

    /// Little-endian byte writers. Explicit so the wire layout is independent of the host.
    inline uint8_t *PutU8(uint8_t *p, uint8_t v) {
      *p = v;
      return p + 1;
    }

    inline uint8_t *PutU16(uint8_t *p, uint16_t v) {
      p[0] = static_cast<uint8_t>(v & 0xFFu);
      p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
      return p + 2;
    }

    inline uint8_t *PutU32(uint8_t *p, uint32_t v) {
      p[0] = static_cast<uint8_t>(v & 0xFFu);
      p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
      p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
      p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
      return p + 4;
    }

    /**
     * Writes a float as its IEEE-754 single-precision bit pattern, little-endian.
     *
     * memcpy rather than a pointer cast: the cast is a strict-aliasing violation that GCC is
     * entitled to miscompile at -O2, and every compiler turns this memcpy into one move.
     */
    inline uint8_t *PutF32(uint8_t *p, float v) {
      uint32_t bits;
      std::memcpy(&bits, &v, sizeof(bits));
      return PutU32(p, bits);
    }

  } // namespace

  uint16_t Crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; ++i) {
      crc ^= static_cast<uint16_t>(data[i]) << 8;
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 0x8000u) ? static_cast<uint16_t>((crc << 1) ^ 0x1021u)
                              : static_cast<uint16_t>(crc << 1);
      }
    }
    return crc;
  }

  size_t SerializeRecord(const TelemetryRecord &record, uint8_t *out) {
    uint8_t *p = out;

    p = PutU32(p, record.seq);
    p = PutU32(p, record.uptime_ms);
    p = PutU32(p, record.uv.timestamp_us);
    p = PutF32(p, record.uv.irradiance_w_m2);
    p = PutF32(p, record.uv.sigma_w_m2);
    p = PutU16(p, record.uv.flags);
    p = PutU8(p, record.uv.face_mask);

    for (size_t face = 0; face < OWLSAT_UV_FACE_COUNT; ++face) {
      p = PutU16(p, record.uv.code[face]);
      p = PutU8(p, record.uv.gain[face]);
      p = PutU16(p, record.uv.face_flags[face]);
    }

    // If this trips, the layout above and OWLSAT_RECORD_WIRE_BYTES have drifted apart, and the
    // frame length the transmit task computes no longer matches what it wrote.
    configASSERT(static_cast<size_t>(p - out) == OWLSAT_RECORD_WIRE_BYTES);
    return static_cast<size_t>(p - out);
  }

  size_t BuildTelemetryFrame(uint32_t frame_seq,
                             const TelemetryRecord *records,
                             size_t count,
                             uint8_t *out) {
    if (records == nullptr || count == 0) {
      return 0;
    }
    if (count > OWLSAT_RECORDS_PER_FRAME) {
      count = OWLSAT_RECORDS_PER_FRAME;
    }

    const size_t payload_len = OWLSAT_BATCH_HEADER_BYTES + (count * OWLSAT_RECORD_WIRE_BYTES);

    uint8_t *p = out;
    p = PutU8(p, OWLSAT_SYNC0);
    p = PutU8(p, OWLSAT_SYNC1);
    p = PutU8(p, OWLSAT_FRAME_VERSION);
    p = PutU8(p, static_cast<uint8_t>(FrameType::TelemetryBatch));
    p = PutU32(p, frame_seq);
    p = PutU16(p, static_cast<uint16_t>(payload_len));

    p = PutU8(p, static_cast<uint8_t>(count));
    for (size_t i = 0; i < count; ++i) {
      p += SerializeRecord(records[i], p);
    }

    const size_t framed = static_cast<size_t>(p - out);
    p = PutU16(p, Crc16(out, framed));

    return static_cast<size_t>(p - out);
  }

} // namespace OwlSat
