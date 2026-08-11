/**

  @file       sxuv5.h
  @brief      Interfacing API for the five-face SXUV5 EUV photodiode array.
  @details    The spacecraft carries five SXUV5 diodes, one per face; the sixth face carries the
              antenna. Each diode measures the solar EUV irradiance *projected onto its own face*,
              so no single diode measures the quantity the science wants. Recovering the true
              normal-incidence irradiance means combining the five projections.

              The API is layered, and each layer is separately callable so that a failure in one
              does not deny access to the one below it:

                L0  Transport   — mux channel select, TIA gain select, ADC frame over SPI.
                L1  Acquisition — SampleRaw() / SampleEUV(): ADC codes, auto-ranged, timestamped.
                L2  Conversion  — codes -> photocurrent -> per-face irradiance, with per-face σ.
                L3  Fusion      — ScaleEUV(): five projections -> one true irradiance + sun vector.

              Raw codes survive to telemetry at every layer. When calibration constants drift or
              turn out to be wrong, raw codes are the only recoverable record
              (docs/internal/sxuv5.md §4).

              Per docs/internal/sxuv5.md §9 the array is instance-free but channel-addressed:
              every call names a Face, and a dead diode degrades to a flagged channel that the
              fusion step drops, never to a failed subsystem.

              @par Threading
              All calls are synchronous and block for the SPI transaction and the front-end
              settling time. They do not delay on a timer or manage cadence — that belongs to
              the FreeRTOS task layer via vTaskDelayUntil against a fixed reference timestamp.
              The driver is *not* internally serialised: if more than one task samples the array,
              the caller owns the mutex, because mux and gain state are global to the chain.

  @author     Viola Case
  @date       11.08.2026
  @copyright  © Viola Case, 2026. All rights reserved.

**/
#pragma once

#include <cstdint>
#include <OwlSat/sxuv5_config.h>

namespace OwlSat {

  // =========================================================================
  // Types
  // =========================================================================

  /**
   * @brief Diode identity. Ordering is the body-frame axis order and indexes every array below.
   *
   * @note There is no MinusZ. That face carries the antenna, which is the single most
   *       consequential fact about this sensor set: the array cannot observe the Z component
   *       of the sun vector when the sun is in the −Z hemisphere. See ScaleEUV().
   */
  enum class Face : uint8_t {
    PlusX  = 0,
    MinusX = 1,
    PlusY  = 2,
    MinusY = 3,
    PlusZ  = 4,
  };

  /// Transimpedance gain step. Gain0 is the lowest gain / largest measurable current.
  enum class Gain : uint8_t {
    Gain0 = 0,
    Gain1 = 1,
    Gain2 = 2,
    Gain3 = 3,
  };

  /// Per-face status. Flags accumulate; a face may be both grazing and an outlier.
  enum SXUV5Flag : uint16_t {
    SXUV5_FLAG_OK          = 0,
    SXUV5_FLAG_SATURATED   = 1u << 0,  ///< Code at or near full scale even at the lowest gain.
    SXUV5_FLAG_UNDERRANGE  = 1u << 1,  ///< Code in the bottom few LSBs even at the highest gain.
    SXUV5_FLAG_DARK        = 1u << 2,  ///< Below the dark threshold — face is not sunlit.
    SXUV5_FLAG_ADC_FAULT   = 1u << 3,  ///< Transport failed; the sample carries no information.
    SXUV5_FLAG_DISABLED    = 1u << 4,  ///< Channel disabled by ground command.
    SXUV5_FLAG_GRAZING     = 1u << 5,  ///< cos(θ) below SXUV5_MIN_COSINE; excluded from the fit.
    SXUV5_FLAG_OUTLIER     = 1u << 6,  ///< Inconsistent with the other faces; dropped from the fit.
    SXUV5_FLAG_AUTORANGE   = 1u << 7,  ///< Auto-range hit its step limit without converging.

    // Reconstruction-level flags, reported in EUVResult::flags.
    SXUV5_FLAG_ECLIPSE       = 1u << 8,  ///< No face is sunlit. Irradiance is not measurable.
    SXUV5_FLAG_Z_UNOBSERVED  = 1u << 9,  ///< Sun is in the −Z hemisphere; the missing face blinds us.
    SXUV5_FLAG_LOWER_BOUND   = 1u << 10, ///< Result is a lower bound, not an estimate. See ScaleEUV().
    SXUV5_FLAG_DEGENERATE    = 1u << 11, ///< Too few usable faces to constrain the fit.
    SXUV5_FLAG_OUT_OF_FAMILY = 1u << 12, ///< Result outside the plausible solar range; suspect the chain.
    SXUV5_FLAG_POOR_FIT      = 1u << 13, ///< χ²/dof large: the faces disagree beyond their stated σ.
  };

  /**
   * @brief One unconverted ADC reading. No scaling, no calibration, no interpretation.
   *
   * This is the downlink-critical record. Everything else in this header can be recomputed
   * on the ground from these fields alone, provided the calibration of the day is known.
   */
  struct RawSample {
    uint32_t code;         ///< ADC code as read, right-aligned.
    Face     face;         ///< Which diode produced it.
    Gain     gain;         ///< Gain in force at conversion — meaningless without this.
    uint32_t timestamp_us; ///< time_us_32() at end of conversion.
    uint16_t flags;        ///< Bitwise OR of SXUV5Flag.
  };

  /// One face converted to physical units.
  struct FaceSample {
    RawSample raw;              ///< The reading this was derived from.
    float     current_a;        ///< Photocurrent, dark-corrected [A].
    float     irradiance_w_m2;  ///< Irradiance *on this face* [W/m²]. Still a cosine projection.
    float     sigma_w_m2;       ///< 1σ uncertainty on the above [W/m²].
  };

  /// One acquisition pass across the whole array.
  struct ArraySample {
    FaceSample face[SXUV5_FACE_COUNT]; ///< Indexed by Face.
    float      temperature_c;          ///< Front-end temperature used for the corrections.
    uint32_t   timestamp_us;           ///< Start of the pass.
  };

  /// How ScaleEUV() arrived at its answer.
  enum class ReconstructMethod : uint8_t {
    None,          ///< No solution (eclipse or degenerate geometry).
    Standalone,    ///< Sun vector solved from the diodes alone.
    AttitudeAided, ///< Weighted least squares against a caller-supplied sun vector.
  };

  /// The science product: true EUV irradiance, reconstructed from the five projections.
  struct EUVResult {
    float             irradiance_w_m2; ///< Normal-incidence band-integrated EUV irradiance [W/m²].
    float             sigma_w_m2;      ///< 1σ uncertainty, statistical and systematic combined.
    float             sun_body[3];     ///< Unit sun vector in body frame. Zero if unsolved.
    float             chi_square;      ///< Fit residual. Zero when dof == 0.
    uint8_t           dof;             ///< Degrees of freedom in the fit (faces used − 1).
    uint8_t           faces_used;      ///< Count of faces that contributed.
    uint8_t           face_mask;       ///< Bit i set if Face(i) contributed.
    uint16_t          flags;           ///< Bitwise OR of SXUV5Flag reconstruction-level flags.
    ReconstructMethod method;
    uint32_t          timestamp_us;    ///< Copied from the ArraySample.
  };


  // =========================================================================
  // Calibration
  // =========================================================================

  /**
   * @brief A piecewise-linear calibration curve.
   *
   * Points are sampled measurements from beamline or thermal-vac calibration; everything in
   * between is linearly interpolated, and queries outside the range clamp to the end points
   * rather than extrapolating. Clamping is deliberate: an extrapolated radiometric correction
   * is a fabricated number, and a flat one at least fails visibly.
   *
   * @c x must be strictly increasing. Storage is owned by the caller and must outlive the curve
   * — in practice these point at const tables in flash, or at a ground-uploaded block in RAM.
   */
  struct Curve {
    const float *x;
    const float *y;
    uint8_t      n; ///< Zero means "no correction"; Interpolate() returns 1.0f.
  };

  /**
   * @brief Everything known about one channel that turns a code into an irradiance.
   *
   * Mutable at runtime so ground can push a revised calibration without a software load.
   */
  struct FaceCal {
    float rf_ohms[SXUV5_GAIN_COUNT];  ///< As-measured feedback resistance per gain step [Ω].
    float offset_v[SXUV5_GAIN_COUNT]; ///< Zero-input offset per gain step [V].
    float responsivity_a_per_w;       ///< Includes any accumulated degradation factor.
    float active_area_m2;
    float dark_current_a;             ///< At SXUV5_DARK_REF_TEMP_C.
    float normal[3];                  ///< Face normal, body frame, unit length.
    Curve angular;                    ///< Angular response vs cos(θ), normalised to 1.0 at normal incidence.
    Curve temperature;                ///< Relative responsivity vs temperature [°C].
    float rel_uncertainty;            ///< Systematic radiometric uncertainty, relative (1σ).
    bool  enabled;                    ///< False marks the channel dead; fusion skips it.
  };

  /**
   * @brief Piecewise-linear interpolation with clamped ends.
   * @param c Curve to evaluate. An empty curve returns 1.0f, the identity correction.
   * @param x Query point.
   * @return Interpolated value.
   */
  float Interpolate(const Curve &c, float x);

  /// @return Mutable calibration for a face. Never null; Face is a closed enum.
  FaceCal &Calibration(Face face);

  /// Restores the compile-time defaults from sxuv5_config.h for every face.
  void ResetCalibration();


  // =========================================================================
  // L0/L1 — transport and acquisition
  // =========================================================================

  /**
   * @brief Hook for the ADC transport, so the driver can be built and tested before the ADC
   *        part number is settled (docs/internal/sxuv5.md §6.4).
   *
   * @c convert is called with the mux channel already selected and the gain already settled,
   * and must return a right-aligned code. Returning false marks the sample SXUV5_FLAG_ADC_FAULT.
   */
  struct AdcTransport {
    bool (*init)();
    bool (*convert)(uint32_t *code_out);
  };

  /// Installs a transport. Pass nullptr members to fall back to the built-in SPI implementation.
  void SetAdcTransport(const AdcTransport &transport);

  /**
   * @brief Supplies the front-end temperature used for dark-current and responsivity correction.
   *
   * Defaults to a constant SXUV5_DARK_REF_TEMP_C, which makes the corrections no-ops. Wire this
   * to the board thermistor nearest the TIA — not to the RP2350 die sensor, which reads its own
   * self-heating and tells you nothing about the analog front end.
   */
  void SetTemperatureProvider(float (*provider)());

  /**
   * @brief Configures GPIO, SPI and the mux/gain lines, and loads default calibration.
   * @return False if the transport failed to initialise. The API remains callable and will
   *         report SXUV5_FLAG_ADC_FAULT per sample.
   */
  bool InitEUV();

  /**
   * @brief Single conversion at an explicit gain. No auto-ranging, no conversion to units.
   *
   * Selects the mux channel, sets the gain, waits the configured settling time, converts.
   * This is the primitive that stays usable when every calibration assumption is in doubt.
   */
  RawSample SampleRawAt(Face face, Gain gain);

  /**
   * @brief Single conversion, auto-ranged.
   *
   * Starts from the gain that last worked for this face and walks toward a code inside
   * [SXUV5_AUTORANGE_LOW, SXUV5_AUTORANGE_HIGH] of full scale, at most
   * SXUV5_AUTORANGE_MAX_STEPS times. Each step costs a settling delay, so the per-face
   * worst case is bounded but not small.
   */
  RawSample SampleRaw(Face face);

  /**
   * @brief Acquires all five faces and converts them to per-face irradiance.
   *
   * Faces are read in enum order. Because the reads are sequential and each carries a settling
   * delay, the pass is not instantaneous; the per-face timestamps record the skew. At the
   * mission's multi-second cadence this is irrelevant, but it matters if the spacecraft is
   * tumbling fast enough to move the sun vector appreciably within a pass.
   *
   * A face that faults is populated with flags set and zeroed physical values, and the pass
   * continues. One dead diode never denies the other four.
   */
  ArraySample SampleEUV();


  // =========================================================================
  // L2 — conversion
  // =========================================================================

  /**
   * @brief Code -> dark-corrected photocurrent [A].
   *
   * Inverts V_out = I_photo · R_f using the as-measured feedback resistance for the gain step
   * recorded in the sample, subtracts the per-gain offset, then subtracts the temperature-scaled
   * dark current (which doubles every SXUV5_DARK_DOUBLING_C).
   */
  float RawToCurrent(const RawSample &raw, float temperature_c);

  /**
   * @brief Photocurrent -> irradiance on that face [W/m²].
   *
   * E_face = I / (R(T) · A). This is still a projection: it is the irradiance falling on that
   * face, i.e. E_true·cos(θ), not the irradiance the science wants.
   */
  float CurrentToFaceIrradiance(float current_a, Face face, float temperature_c);

  /**
   * @brief 1σ uncertainty on a face irradiance [W/m²].
   *
   * Combines quantisation (LSB/√12), ADC noise, Johnson noise of R_f over the front-end noise
   * bandwidth, and the systematic radiometric and resistor-tolerance terms, in quadrature.
   */
  float FaceSigma(const FaceSample &sample, float temperature_c);

  /// Converts one raw sample into a full FaceSample. Exposed for replaying downlinked raws.
  FaceSample ConvertSample(const RawSample &raw, float temperature_c);


  // =========================================================================
  // L3 — fusion: the actual reconstruction
  // =========================================================================

  /**
   * @brief Reconstructs true EUV irradiance from the five face projections, unaided.
   *
   * Each face measures E_face,i = E_true · max(0, n_i · s). Writing v = E_true·s, the readings
   * are linear in v and the axis-aligned normals make the solve separable:
   *
   *     v_x = E(+X) − E(−X)
   *     v_y = E(+Y) − E(−Y)
   *     v_z = E(+Z)                    ← only when +Z is sunlit
   *
   * Then E_true = ‖v‖ and s = v/‖v‖. Differencing each opposed pair is exact because at most
   * one face of a pair can be sunlit, and it cancels common-mode offset in the dark partner.
   *
   * @warning The missing −Z face is not a small gap. When the sun is anywhere in the −Z
   *          hemisphere, +Z reads dark and v_z is unconstrained: the reading bounds it as
   *          negative but places no bound on its magnitude. The result is then
   *          √(v_x² + v_y²), which is a **lower bound on E_true, not an estimate**, and is
   *          flagged SXUV5_FLAG_Z_UNOBSERVED | SXUV5_FLAG_LOWER_BOUND. Consumers must branch
   *          on that flag rather than treating the number as a measurement. Roughly half the
   *          sky is affected, so this is the common case, not a corner case — which is the
   *          argument for calling ScaleEUV(sample, sun_body) instead whenever attitude
   *          knowledge exists.
   *
   * @param sample One acquisition pass.
   * @return Reconstruction with method Standalone, or None under eclipse.
   */
  EUVResult ScaleEUV(const ArraySample &sample);

  /**
   * @brief Reconstructs true EUV irradiance given an independent sun direction.
   *
   * With s known, every sunlit face is an independent measurement of the one remaining unknown
   * E_true, and the solve becomes a weighted least squares over the faces with
   * cos(θ_i) ≥ SXUV5_MIN_COSINE:
   *
   *     E = Σ(c_i·E_i/σ_i²) / Σ(c_i²/σ_i²),     σ_E = 1/√(Σ c_i²/σ_i²)
   *
   * where c_i = (n_i·s) corrected by the face's measured angular response. This is strictly
   * better than the standalone solve: it works everywhere on the sky including the −Z
   * hemisphere, it averages down noise across faces instead of taking one face's word for an
   * axis, and the residual χ² becomes a genuine consistency check. A face whose normalised
   * residual exceeds SXUV5_OUTLIER_SIGMA is flagged, dropped, and the fit is repeated once —
   * which is how a slowly degrading diode announces itself long before it fails outright.
   *
   * @param sample   One acquisition pass.
   * @param sun_body Unit sun vector in body frame, from the ADCS. Normalised internally;
   *                 a zero-length vector falls back to the standalone solve.
   */
  EUVResult ScaleEUV(const ArraySample &sample, const float sun_body[3]);

  /**
   * @brief 1σ uncertainty of a reconstruction [W/m²].
   *
   * A plain accessor — the uncertainty is computed during the fit, where the weights and the
   * dropped faces are known, and cannot be reconstructed afterwards from the result alone.
   */
  float GetEUVUncertainty(const EUVResult &result);

} // namespace OwlSat
