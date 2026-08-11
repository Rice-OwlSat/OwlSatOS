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
// ADC  (TBC — docs/internal/sxuv5.md §6.4: part number, resolution and V_ref unconfirmed)
// ---------------------------------------------------------------------------

/// ADC resolution in bits.
#define SXUV5_ADC_BITS 16

/// Bytes clocked out per conversion frame.
#define SXUV5_ADC_FRAME_BYTES 2

/// Right-shift applied to the assembled frame to right-align the code (MSB-aligned parts need this).
#define SXUV5_ADC_SHIFT 0

/// ADC reference voltage [V].
#define SXUV5_ADC_VREF_V 2.5f

/// Set to 1 for a bipolar/two's-complement output part, 0 for straight binary.
#define SXUV5_ADC_SIGNED 0

/// RMS noise referred to the ADC input [V]. Used by the uncertainty model.
#define SXUV5_ADC_NOISE_VRMS 3.0e-5f

/// SPI clock [Hz].
#define SXUV5_SPI_BAUD 2000000u

/// Full-scale code, derived.
#define SXUV5_ADC_FULL_SCALE ((1u << SXUV5_ADC_BITS) - 1u)


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
 * Nominal band-integrated solar EUV irradiance at 1 AU [W/m²].
 *
 * Sanity bound only — a reconstruction wildly outside this is flagged, never clamped.
 */
#define SXUV5_NOMINAL_IRRADIANCE_W_M2 1.0e-2f

/// Reconstructions outside NOMINAL × [1/this, this] raise SXUV5_FLAG_OUT_OF_FAMILY.
#define SXUV5_FAMILY_RATIO 10.0f
