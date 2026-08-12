/**

  @file       sxuv5_config.h
  @brief      Compile-time configuration for the SXUV5 EUV photodiode array.
  @details    Every constant that describes the analog front end, the ADC, or the diodes lives
              here and nowhere else. Per docs/internal/sxuv5.md §4, conversion constants are
              compile-time configuration, not literals buried in the conversion path.

              Values marked TBC are placeholders standing in for the blocking unknowns listed
              in docs/internal/sxuv5.md §6. They are dimensionally correct and order-of-magnitude
              plausible, so the driver compiles, runs, and can be unit-tested, but no number
              here is flight-valid until the hardware lead confirms it.
  @author     Viola Case
  @date       11.08.2026
  @copyright  © Viola Case, 2026. All rights reserved.

**/
#pragma once

  #ifndef SXUV5_CONSTANTS_CONFIRMED
    #pragma message("SXUV5: building with PLACEHOLDER calibration constants — see sxuv5_config.h")
  #endif


// ---------------------------------------------------------------------------
// Array geometry
// ---------------------------------------------------------------------------

/// Number of populated diode faces. The sixth face carries the antenna.
#define SXUV5_FACE_COUNT 5

/// Number of selectable transimpedance gains (2 GPIOs => 4 states).
#define SXUV5_GAIN_COUNT 4


// ---------------------------------------------------------------------------
// ADC — TI ADS7828 (block diagram, "ADC options: ADS7828 (from AMSAT)")
//
// 8-channel 12-bit SAR with an internal 2.5 V reference on I2C. One command byte selects
// the channel and the power-down mode; the conversion comes back as two bytes, four leading
// zeros then D11..D0. Only one input channel is used: the mux upstream of the TIA does the
// per-diode selection, so the ADC sees a single TIA output.
// ---------------------------------------------------------------------------

/// ADC resolution in bits.
#define SXUV5_ADC_BITS 12

/// Bytes read back per conversion.
#define SXUV5_ADC_FRAME_BYTES 2

/// Reference voltage [V]. The ADS7828's internal reference; full scale, not the 3.3 V rail.
#define SXUV5_ADC_VREF_V 2.5f

/**
 * RMS noise referred to the ADC input [V].
 *
 * Well under an LSB (610 µV) for a part this coarse, so the uncertainty model is quantisation-
 * dominated at the ADC and Johnson-dominated at the TIA. Kept as a term so a noisier part
 * substituted later shows up in σ instead of silently flattering it.
 */
#define SXUV5_ADC_NOISE_VRMS 1.0e-4f

/// Single-ended input the TIA output lands on (0–7).
#define SXUV5_ADC_CHANNEL 0

/**
 * ADS7828 command-byte power-down field (PD1:PD0, bits 3:2).
 *
 * 0b11 keeps the internal reference and the converter both on. Powering either down between
 * conversions saves a rail already gated by SENS_PWR, and would cost a reference settling
 * delay on every single sample.
 */
#define SXUV5_ADC_PD_MODE 0x3u

/// I2C clock [Hz]. Fast mode; the bus is shared, so this is a bus-wide decision.
#define SXUV5_I2C_BAUD 400000u

/// Per-transfer I2C timeout [µs]. Bounds a wedged or unpowered slave into an ADC_FAULT.
#define SXUV5_I2C_TIMEOUT_US 5000u

/// Full-scale code, derived.
#define SXUV5_ADC_FULL_SCALE ((1u << SXUV5_ADC_BITS) - 1u)


// ---------------------------------------------------------------------------
// Sensor rail  (SENS_PWR — dual low-power switch)
// ---------------------------------------------------------------------------

/**
 * Delay from asserting SENS_PWR to a trustworthy conversion [µs]. (TBC)
 *
 * Bounded below by the ADS7828's internal reference charging its bypass capacitor, and above
 * by the TIA: at R_f = 1 GΩ the front end needs ~10τ ≈ 5 ms to forget its power-on transient,
 * and the coax capacitance the mux presents does not help. Budgeted long deliberately — this
 * is paid once per power cycle, not once per sample.
 */
#define SXUV5_SENS_PWR_SETTLE_US 20000u


// ---------------------------------------------------------------------------
// Transimpedance front end  (TBC — docs/internal/sxuv5.md §6.1)
// ---------------------------------------------------------------------------

/// Feedback resistance per gain step [Ω], index 0 = lowest gain (largest signal range).
#define SXUV5_RF_OHMS_INIT { 1.0e6f, 1.0e7f, 1.0e8f, 1.0e9f }

/// Relative tolerance of the feedback resistors (systematic, per gain step).
#define SXUV5_RF_TOLERANCE 0.005f

/**
 * Settling time per gain step [µs], applied after any mux or gain change.
 *
 * Dominated by R_f·C_f plus analog-switch charge injection into the summing node.
 * At R_f = 100 MΩ with C_f = 0.5 pF, τ ≈ 50 µs, so ~10τ is budgeted. These must be
 * re-measured once the compensation network is fixed (docs/internal/sxuv5.md §7).
 */
#define SXUV5_SETTLE_US_INIT { 200u, 500u, 1500u, 5000u }

/// Effective noise bandwidth of the front end [Hz], used for the Johnson-noise term.
#define SXUV5_NOISE_BANDWIDTH_HZ 4700.0f

/// Assumed front-end temperature for the Johnson-noise term [K].
#define SXUV5_FRONTEND_TEMP_K 300.0f


// ---------------------------------------------------------------------------
// Diode  (datasheet rev. 2024-10-10 — these are the trustworthy numbers)
// ---------------------------------------------------------------------------

/**
 * Responsivity [A/W], flat across the EUV band.
 *
 * Each absorbed photon liberates E_photon/w pairs with w ≈ 3.63 eV, giving
 * R ≈ q/w ≈ 0.27 A/W independent of wavelength, except at absorption edges
 * (Si L-edge ≈ 12.4 nm). Band-edge structure is a ground-segment correction.
 */
#define SXUV5_RESPONSIVITY_A_PER_W 0.27f

/// Active area [m²]. 5 mm², Ø2.5 mm circular.
#define SXUV5_ACTIVE_AREA_M2 5.0e-6f

/// Dark current at the reference temperature [A]. (TBC — depends on the confirmed bias mode.)
#define SXUV5_DARK_CURRENT_25C_A 1.0e-12f

/// Reference temperature for the dark-current model [°C].
#define SXUV5_DARK_REF_TEMP_C 25.0f

/// Dark current doubles every this many °C (datasheet range 8–10 °C).
#define SXUV5_DARK_DOUBLING_C 9.0f

/**
 * Expected photocurrent from a fully sunlit face [A].
 *
 * ~1.3 nA for most sun spectra, per the block diagram's estimate from the LISIRD TIMED SEE SSI
 * instrument. This is what sizes the gain ladder: at R_f = 1 GΩ it lands near half of the
 * ADS7828's 2.5 V full scale, which is why SXUV5_DEFAULT_GAIN seeds the auto-range there.
 */
#define SXUV5_NOMINAL_PHOTOCURRENT_A 1.3e-9f

/// Radiometric calibration uncertainty per face, relative (1σ).
#define SXUV5_CAL_REL_UNCERTAINTY 0.02f

/// Face-normal alignment uncertainty [rad], folded into the cosine-projection error.
#define SXUV5_ALIGNMENT_SIGMA_RAD 0.017f


// ---------------------------------------------------------------------------
// Acquisition policy
// ---------------------------------------------------------------------------

/// Auto-range steps down a gain above this fraction of full scale.
#define SXUV5_AUTORANGE_HIGH 0.90f

/// Auto-range steps up a gain below this fraction of full scale.
#define SXUV5_AUTORANGE_LOW 0.06f

/// Maximum gain changes per auto-ranged read, bounding worst-case read latency.
#define SXUV5_AUTORANGE_MAX_STEPS 4

/**
 * Gain the auto-range starts from on a cold face, as an index into the R_f ladder.
 *
 * The highest gain, because SXUV5_NOMINAL_PHOTOCURRENT_A lands mid-scale there and a sunlit
 * face is the expected case. Starting lower would cost an extra settle — and the settles are
 * the expensive part of a pass — on nearly every sample.
 */
#define SXUV5_DEFAULT_GAIN (SXUV5_GAIN_COUNT - 1)


// ---------------------------------------------------------------------------
// Reconstruction policy
// ---------------------------------------------------------------------------

/**
 * Minimum cos(θ) for a face to contribute to the fit (≈ 80° incidence).
 *
 * Below this the cosine projection is dominated by alignment error and by the
 * deviation of the real angular response from an ideal cosine.
 */
#define SXUV5_MIN_COSINE 0.17f

/// A face reading below this irradiance is treated as dark [W/m²].
#define SXUV5_DARK_THRESHOLD_W_M2 1.0e-5f

/// Normalised residual above which a face is flagged as an outlier and dropped.
#define SXUV5_OUTLIER_SIGMA 3.0f

/**
 * Nominal band-integrated solar EUV irradiance at 1 AU [W/m²]. (Derived)
 *
 * Derived from the same ~1.3 nA photocurrent estimate that sizes the gain ladder, so the
 * family check is centred on the signal the front end was designed for and the two numbers
 * cannot disagree. ≈ 9.6e-4 W/m² with the current constants; an independently sourced
 * literal here once sat 10× above this and put the nominal signal just outside the band.
 *
 * Sanity bound only — a reconstruction wildly outside this is flagged, never clamped.
 */
#define SXUV5_NOMINAL_IRRADIANCE_W_M2 \
  (SXUV5_NOMINAL_PHOTOCURRENT_A / (SXUV5_RESPONSIVITY_A_PER_W * SXUV5_ACTIVE_AREA_M2))

/// Reconstructions outside NOMINAL × [1/this, this] raise SXUV5_FLAG_OUT_OF_FAMILY.
#define SXUV5_FAMILY_RATIO 10.0f
