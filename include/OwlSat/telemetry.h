/**

  @file       telemetry.h
  @brief      Science record and downlink frame format for OwlSatOS.
  @details    Two things live here: the record the sensor task writes into the storage table,
              and the wire frame the transmit task packs those records into.

              @par The record keeps raw codes
              A record carries the reconstructed irradiance *and* the per-face ADC codes and the
              gain each was taken at. The reconstruction is the science product, but it is a
              function of calibration constants that are still marked TBC and that ground may
              revise after launch. Raw codes are the only part of a record that stays correct
              when the calibration turns out to be wrong, so they are downlinked, not discarded.

              @par The wire format is explicit, not a struct dump
              Records are serialised field by field, little-endian, into a fixed
              OWLSAT_RECORD_WIRE_BYTES layout. Nothing on the wire depends on this compiler's
              struct padding or on the ground station being built the same way. The cost is one
              serializer; the alternative is a format that changes when someone reorders a field.

  @warning    The framing below is provisional. The README makes agreeing it with the comms team
              a precondition for the telemetry task, and the AMSAT LTM-1 is reached over CAN via
              an SPI bridge, which may impose its own packetisation (AX.25 is the likely
              standard). Treat OwlSatFrame as the shape of a frame, not as the agreed frame.

  @author     Viola Case
  @date       29.08.2026
  @copyright  © Viola Case, 2026. All rights reserved.

**/
#pragma once

#include <cstddef>
#include <cstdint>

#include <OwlSat/config.h>

namespace OwlSat {

  // =========================================================================
  // Science record
  // =========================================================================

  /**
   * @brief One acquisition pass across the EUV array, as stored and downlinked.
   *
   * Field-for-field a compaction of the sensor branch's ArraySample plus its EUVResult: the
   * per-face raws that cannot be recomputed, and the reconstruction that can. Everything the
   * driver reports that is derivable on the ground from these fields — photocurrent, per-face
   * irradiance, the sun vector, χ² — is left out of the record on purpose.
   */
  struct UvSample {
    /// Right-aligned ADC code per face. 12-bit part; uint16_t holds any code up to 16 bits.
    uint16_t code[OWLSAT_UV_FACE_COUNT];
    /// Gain step in force per face. A code without its gain is meaningless.
    uint8_t  gain[OWLSAT_UV_FACE_COUNT];
    /// Per-face SXUV5Flag bits — saturation, dark, fault, disabled.
    uint16_t face_flags[OWLSAT_UV_FACE_COUNT];
    /// Reconstructed normal-incidence irradiance [W/m²]. Zero when unsolved.
    float    irradiance_w_m2;
    /// 1σ uncertainty on the above [W/m²].
    float    sigma_w_m2;
    /// Reconstruction-level SXUV5Flag bits. LOWER_BOUND here means irradiance is a bound.
    uint16_t flags;
    /// Faces that contributed to the reconstruction, bit i = Face(i).
    uint8_t  face_mask;
    /// time_us_32() at the start of the pass.
    uint32_t timestamp_us;
  };

  /// One row of the storage table.
  struct TelemetryRecord {
    /// Monotonic, assigned by the storage table on append. Identifies the record everywhere.
    uint32_t seq;
    /// Tick count at append, i.e. uptime in ms. Wall time is the GPS/RTC's to supply later.
    uint32_t uptime_ms;
    UvSample uv;
  };


  // =========================================================================
  // Wire format
  // =========================================================================

  /// Frame sync bytes, 'O' 'W'.
  constexpr uint8_t  OWLSAT_SYNC0 = 0x4F;
  constexpr uint8_t  OWLSAT_SYNC1 = 0x57;

  /// Frame format version. Bump on any layout change; ground branches on it.
  constexpr uint8_t  OWLSAT_FRAME_VERSION = 1;

  /// Frame types.
  enum class FrameType : uint8_t {
    TelemetryBatch = 0x01, ///< Payload is a record count followed by that many wire records.
    Status         = 0x02, ///< Reserved for the health/status reply. Not yet emitted.
  };

  /**
   * @brief Frame header, on the wire, little-endian.
   *
   * @verbatim
   *   off  size  field
   *   0    2     sync            0x4F 0x57
   *   2    1     version         OWLSAT_FRAME_VERSION
   *   3    1     type            FrameType
   *   4    4     frame_seq       monotonic, per-frame, independent of record seq
   *   8    2     payload_len     bytes of payload following the header
   *   10   n     payload
   *   10+n 2     crc16           CRC-16/CCITT-FALSE over bytes [0, 10+n)
   * @endverbatim
   */
  constexpr size_t OWLSAT_FRAME_HEADER_BYTES = 10;
  constexpr size_t OWLSAT_FRAME_CRC_BYTES    = 2;

  /// Largest frame this build can emit [bytes].
  constexpr size_t OWLSAT_FRAME_MAX_BYTES =
      OWLSAT_FRAME_HEADER_BYTES + OWLSAT_PACKET_MAX_PAYLOAD + OWLSAT_FRAME_CRC_BYTES;

  /**
   * @brief Serialised size of one record [bytes].
   *
   * seq(4) + uptime(4) + irradiance(4) + sigma(4) + flags(2) + face_mask(1)
   *        + timestamp(4) + 5 x (code(2) + gain(1) + face_flags(2))
   */
  constexpr size_t OWLSAT_RECORD_WIRE_BYTES = 23 + (5 * OWLSAT_UV_FACE_COUNT);

  /// A TelemetryBatch payload opens with a one-byte record count.
  constexpr size_t OWLSAT_BATCH_HEADER_BYTES = 1;

  /// Records that fit in one frame at the configured payload limit.
  constexpr size_t OWLSAT_RECORDS_PER_FRAME =
      (OWLSAT_PACKET_MAX_PAYLOAD - OWLSAT_BATCH_HEADER_BYTES) / OWLSAT_RECORD_WIRE_BYTES;

  static_assert(OWLSAT_RECORDS_PER_FRAME >= 1,
                "OWLSAT_PACKET_MAX_PAYLOAD cannot hold a single record");


  // =========================================================================
  // Serialisation
  // =========================================================================

  /**
   * @brief CRC-16/CCITT-FALSE. Poly 0x1021, init 0xFFFF, no reflection, no final xor.
   *
   * Chosen because it is the checksum the ground-station toolchains around AX.25 already have,
   * not for any property of the data. If the comms team names a different one, this is the only
   * place it changes.
   */
  uint16_t Crc16(const uint8_t *data, size_t len);

  /**
   * @brief Writes one record into @p out in wire layout.
   * @param record Record to serialise.
   * @param out    Buffer of at least OWLSAT_RECORD_WIRE_BYTES.
   * @return Bytes written, always OWLSAT_RECORD_WIRE_BYTES.
   */
  size_t SerializeRecord(const TelemetryRecord &record, uint8_t *out);

  /**
   * @brief Builds a complete TelemetryBatch frame from up to OWLSAT_RECORDS_PER_FRAME records.
   *
   * @param frame_seq Per-frame sequence number, owned by the caller.
   * @param records   Records to pack, in order.
   * @param count     How many; clamped to OWLSAT_RECORDS_PER_FRAME.
   * @param out       Buffer of at least OWLSAT_FRAME_MAX_BYTES.
   * @return Total frame length in bytes, or 0 if @p count was zero.
   */
  size_t BuildTelemetryFrame(uint32_t frame_seq,
                             const TelemetryRecord *records,
                             size_t count,
                             uint8_t *out);

} // namespace OwlSat
