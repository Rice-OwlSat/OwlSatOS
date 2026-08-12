/**

  @file       sxuv5_reconstruct.cpp
  @brief      Reconstruction of true EUV irradiance from the five SXUV5 face projections.
  @details    Layer L3 of the interface described in OwlSat/sxuv5.h.

              Every diode measures E_true·cos(θ_i), never E_true. Recovering the science quantity
              is a geometry problem, and the geometry is lopsided: the array covers five of six
              faces, so the +Z axis is observable only from one side. Both solvers below are
              written to say so explicitly rather than to return a confident-looking number.
  @author     Viola Case
  @date       11.08.2026
  @copyright  © Viola Case, 2026. All rights reserved.

**/

#include <cstdint>
#include <cmath>
#include <OwlSat/sxuv5.h>

namespace OwlSat {

  namespace {

    inline uint8_t Index(Face face) { return static_cast<uint8_t>(face); }

    /// Smallest σ the fit will weight against, so one optimistic channel cannot dominate.
    constexpr float kSigmaFloor = 1.0e-9f;

    /// A channel carries no usable information if it faulted, was disabled, or was unpowered.
    bool Usable(const FaceSample &sample) {
      return (sample.raw.flags & SXUV5_FLAGS_NO_DATA) == 0 && Calibration(sample.raw.face).enabled;
    }

    bool Lit(const FaceSample &sample) {
      return Usable(sample) && sample.irradiance_w_m2 >= SXUV5_DARK_THRESHOLD_W_M2;
    }

    float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

    /// Flags a reconstruction that no plausible solar EUV level can explain.
    void CheckFamily(EUVResult &result) {
      constexpr float kHigh = SXUV5_NOMINAL_IRRADIANCE_W_M2 * SXUV5_FAMILY_RATIO;
      constexpr float kLow  = SXUV5_NOMINAL_IRRADIANCE_W_M2 / SXUV5_FAMILY_RATIO;

      if (result.irradiance_w_m2 > kHigh || result.irradiance_w_m2 < kLow) {
        result.flags |= SXUV5_FLAG_OUT_OF_FAMILY;
      }
    }

    /**
     * Resolves one opposed face pair into a signed component of v = E_true·s.
     *
     * At most one face of a pair can be sunlit, so the difference of the two readings is the
     * component itself, with the dark partner's residual offset cancelling out of it.
     *
     * @return True if the component is observed. False means the pair could not constrain the
     *         axis at all — both channels dead, or the only surviving one reading dark, which
     *         fixes the component's sign but leaves its magnitude free.
     */
    bool ResolveAxis(const FaceSample &positive, const FaceSample &negative, float *value, float *sigma) {
      const bool pos_ok = Usable(positive);
      const bool neg_ok = Usable(negative);

      if (!pos_ok && !neg_ok) return false;

      if (pos_ok && neg_ok) {
        *value = positive.irradiance_w_m2 - negative.irradiance_w_m2;
        *sigma = sqrtf(positive.sigma_w_m2 * positive.sigma_w_m2 + negative.sigma_w_m2 * negative.sigma_w_m2);
        return true;
      }

      // Only one face of the pair survives. It constrains the axis only while it is the lit one.
      const FaceSample &survivor = pos_ok ? positive : negative;
      if (!Lit(survivor)) return false;

      *value = pos_ok ? survivor.irradiance_w_m2 : -survivor.irradiance_w_m2;
      *sigma = survivor.sigma_w_m2;
      return true;
    }

  } // namespace


  // =========================================================================
  // Standalone reconstruction
  // =========================================================================

  EUVResult ScaleEUV(const ArraySample &sample) {
    EUVResult result {};
    result.method       = ReconstructMethod::None;
    result.timestamp_us = sample.timestamp_us;

    // Eclipse first: with nothing lit there is no irradiance to reconstruct, and every
    // expression below would be reporting noise.
    bool any_usable = false;
    bool any_lit    = false;
    for (uint8_t i = 0; i < SXUV5_FACE_COUNT; ++i) {
      if (Usable(sample.face[i])) any_usable = true;
      if (Lit(sample.face[i])) any_lit = true;
    }

    if (!any_usable) {
      result.flags |= SXUV5_FLAG_DEGENERATE;
      return result;
    }
    if (!any_lit) {
      result.flags |= SXUV5_FLAG_ECLIPSE;
      return result;
    }

    // v = E_true·s, one component per body axis.
    float v[3]     = { 0.0f, 0.0f, 0.0f };
    float sigma[3] = { 0.0f, 0.0f, 0.0f };
    bool  observed[3];

    observed[0] = ResolveAxis(sample.face[Index(Face::PlusX)], sample.face[Index(Face::MinusX)], &v[0], &sigma[0]);
    observed[1] = ResolveAxis(sample.face[Index(Face::PlusY)], sample.face[Index(Face::MinusY)], &v[1], &sigma[1]);

    // Z has no opposed partner — the −Z face is the antenna. The +Z diode alone observes the
    // axis only while it is sunlit; the moment it goes dark all we learn is v_z ≤ 0.
    const FaceSample &plus_z = sample.face[Index(Face::PlusZ)];
    if (Usable(plus_z) && Lit(plus_z)) {
      v[2]        = plus_z.irradiance_w_m2;
      sigma[2]    = plus_z.sigma_w_m2;
      observed[2] = true;
    } else {
      observed[2] = false;
      if (Usable(plus_z)) result.flags |= SXUV5_FLAG_Z_UNOBSERVED;
    }

    float magnitude_sq = 0.0f;
    float variance     = 0.0f;

    for (uint8_t axis = 0; axis < 3; ++axis) {
      if (!observed[axis]) {
        v[axis] = 0.0f;
        result.flags |= SXUV5_FLAG_LOWER_BOUND;
        continue;
      }
      magnitude_sq += v[axis] * v[axis];
    }

    const float magnitude = sqrtf(magnitude_sq);

    // σ_E = ‖∂E/∂v · σ_v‖ with ∂E/∂v_i = v_i/E.
    if (magnitude > 0.0f) {
      for (uint8_t axis = 0; axis < 3; ++axis) {
        if (!observed[axis]) continue;
        const float partial = v[axis] / magnitude;
        variance += partial * partial * sigma[axis] * sigma[axis];
      }
    } else {
      for (uint8_t axis = 0; axis < 3; ++axis) variance += sigma[axis] * sigma[axis];
    }

    result.irradiance_w_m2 = magnitude;
    result.sigma_w_m2      = sqrtf(variance);
    result.method          = ReconstructMethod::Standalone;

    // The sun direction is only meaningful once every axis is pinned down. Reporting a unit
    // vector built from a zeroed unobserved axis would point confidently into the equator.
    if (observed[0] && observed[1] && observed[2] && magnitude > 0.0f) {
      result.sun_body[0] = v[0] / magnitude;
      result.sun_body[1] = v[1] / magnitude;
      result.sun_body[2] = v[2] / magnitude;
    }

    for (uint8_t i = 0; i < SXUV5_FACE_COUNT; ++i) {
      if (!Lit(sample.face[i])) continue;
      result.face_mask |= static_cast<uint8_t>(1u << i);
      ++result.faces_used;
    }

    // The standalone solve consumes exactly as many measurements as it has unknowns, so there is
    // nothing left over to test the fit against. χ² stays zero and dof stays zero: this solver
    // cannot detect a drifting diode. The attitude-aided overload can.
    if (result.flags & SXUV5_FLAG_LOWER_BOUND) return result; // A bound is not in or out of family.

    CheckFamily(result);
    return result;
  }


  // =========================================================================
  // Attitude-aided reconstruction
  // =========================================================================

  EUVResult ScaleEUV(const ArraySample &sample, const float sun_body[3]) {
    EUVResult result {};
    result.method       = ReconstructMethod::None;
    result.timestamp_us = sample.timestamp_us;

    if (sun_body == nullptr) return ScaleEUV(sample);

    const float length = sqrtf(Dot(sun_body, sun_body));
    if (!(length > 0.0f) || !std::isfinite(length)) return ScaleEUV(sample); // No attitude; fall back.

    const float s[3] = { sun_body[0] / length, sun_body[1] / length, sun_body[2] / length };

    // Projection and per-face weight for every face the geometry says should be sunlit.
    float   cosine[SXUV5_FACE_COUNT] = { 0.0f };
    float   sigma[SXUV5_FACE_COUNT]  = { 0.0f };
    bool    in_fit[SXUV5_FACE_COUNT] = { false };
    uint8_t candidates               = 0;
    bool    any_lit                  = false;

    for (uint8_t i = 0; i < SXUV5_FACE_COUNT; ++i) {
      const FaceSample &face = sample.face[i];
      if (!Usable(face)) continue;
      if (Lit(face)) any_lit = true;

      const FaceCal &cal = Calibration(static_cast<Face>(i));
      const float    raw_cosine = Dot(cal.normal, s);
      if (raw_cosine < SXUV5_MIN_COSINE) {
        // Excluded from the fit. Back-lit faces are the normal case — at least one face of
        // every opposed pair points away from the sun — so a flag raised for every exclusion
        // would be raised on every pass and mean nothing. The report-worthy case is a face
        // *reading light* where geometry says it cannot see the sun: that disagreement is a
        // wrong attitude, a wrong face normal, or a channel measuring something else.
        if (Lit(face)) result.flags |= SXUV5_FLAG_GRAZING;
        continue;
      }

      // Real diodes deviate from an ideal cosine near the rim of their acceptance. The measured
      // angular response is a calibration curve, interpolated here rather than assumed flat.
      cosine[i] = raw_cosine * Interpolate(cal.angular, raw_cosine);

      // Alignment error enters through the projection: a δθ error in the face normal moves the
      // implied irradiance by E·tanθ·δθ, which blows up exactly where cos θ gets small.
      const float sin_theta = sqrtf(fmaxf(0.0f, 1.0f - raw_cosine * raw_cosine));
      const float align     = face.irradiance_w_m2 * (sin_theta / fmaxf(raw_cosine, SXUV5_MIN_COSINE)) *
                          SXUV5_ALIGNMENT_SIGMA_RAD;

      sigma[i]  = fmaxf(sqrtf(face.sigma_w_m2 * face.sigma_w_m2 + align * align), kSigmaFloor);
      in_fit[i] = true;
      ++candidates;
    }

    if (candidates == 0) {
      // No face is pointed well enough at the sun to measure it. With five faces this happens
      // only in a narrow cone about −Z, where the antenna face would have been looking — a far
      // smaller blind region than the standalone solver's missing half-sky, but still real.
      result.flags |= any_lit ? SXUV5_FLAG_DEGENERATE : SXUV5_FLAG_ECLIPSE;
      return result;
    }

    // Weighted least squares over the single unknown E_true, repeated at most once after
    // discarding the worst outlier.
    float estimate = 0.0f;
    float variance = 0.0f;

    for (uint8_t pass = 0; pass < 2; ++pass) {
      float numerator   = 0.0f;
      float denominator = 0.0f;

      for (uint8_t i = 0; i < SXUV5_FACE_COUNT; ++i) {
        if (!in_fit[i]) continue;
        const float weight = 1.0f / (sigma[i] * sigma[i]);
        numerator += cosine[i] * sample.face[i].irradiance_w_m2 * weight;
        denominator += cosine[i] * cosine[i] * weight;
      }

      if (denominator <= 0.0f) {
        result.flags |= SXUV5_FLAG_DEGENERATE;
        return result;
      }

      estimate = numerator / denominator;
      variance = 1.0f / denominator;

      // Residuals: each fitted face predicts E·cos θ and should reproduce its own reading.
      float   chi_square    = 0.0f;
      uint8_t used          = 0;
      float   worst         = 0.0f;
      int8_t  worst_face    = -1;

      for (uint8_t i = 0; i < SXUV5_FACE_COUNT; ++i) {
        if (!in_fit[i]) continue;
        const float residual   = (sample.face[i].irradiance_w_m2 - estimate * cosine[i]) / sigma[i];
        chi_square += residual * residual;
        ++used;

        const float magnitude = fabsf(residual);
        if (magnitude > worst) {
          worst      = magnitude;
          worst_face = static_cast<int8_t>(i);
        }
      }

      result.chi_square = chi_square;
      result.dof        = used > 0 ? static_cast<uint8_t>(used - 1) : 0;
      result.faces_used = used;

      result.face_mask = 0;
      for (uint8_t i = 0; i < SXUV5_FACE_COUNT; ++i) {
        if (in_fit[i]) result.face_mask |= static_cast<uint8_t>(1u << i);
      }

      // Rejecting an outlier needs at least three faces: with two, the fit cannot tell which of
      // them is the liar, and dropping either one leaves an unfalsifiable answer.
      const bool can_reject = pass == 0 && used >= 3 && worst > SXUV5_OUTLIER_SIGMA && worst_face >= 0;
      if (!can_reject) {
        if (result.dof > 0 && chi_square > static_cast<float>(result.dof) * SXUV5_OUTLIER_SIGMA) {
          result.flags |= SXUV5_FLAG_POOR_FIT;
        }
        break;
      }

      in_fit[worst_face] = false;
      result.flags |= SXUV5_FLAG_OUTLIER;
    }

    // Geometry said these faces should be sunlit and every one of them came back dark. That is
    // eclipse (or an occultation), not a measurement of zero irradiance, and the difference
    // matters to anything that averages or trends the product.
    bool fitted_lit = false;
    for (uint8_t i = 0; i < SXUV5_FACE_COUNT; ++i) {
      if (in_fit[i] && Lit(sample.face[i])) fitted_lit = true;
    }
    if (!fitted_lit) result.flags |= SXUV5_FLAG_ECLIPSE;

    // The statistical σ above averages down across faces, but the radiometric calibration error
    // is common-mode across the array and does not. Floor the result with it rather than letting
    // more faces manufacture accuracy the calibration cannot support.
    const float common_mode = estimate * SXUV5_CAL_REL_UNCERTAINTY;

    result.irradiance_w_m2 = estimate;
    result.sigma_w_m2      = sqrtf(variance + common_mode * common_mode);
    result.method          = ReconstructMethod::AttitudeAided;
    result.sun_body[0]     = s[0];
    result.sun_body[1]     = s[1];
    result.sun_body[2]     = s[2];

    // An eclipsed array reading ~zero is the chain working correctly, not a reading out of
    // family — mirroring how the standalone solver exempts a lower bound. OUT_OF_FAMILY is
    // reserved for measurements that claim to be measurements.
    if (!(result.flags & SXUV5_FLAG_ECLIPSE)) CheckFamily(result);
    return result;
  }


  float GetEUVUncertainty(const EUVResult &result) { return result.sigma_w_m2; }

} // namespace OwlSat
