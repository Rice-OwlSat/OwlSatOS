/**

  @file       sxuv5_interface.cpp
  @brief      Transport, acquisition and unit conversion for the SXUV5 EUV photodiode array.
  @details    Layers L0–L2 of the interface described in OwlSat/sxuv5.h. The reconstruction that
              turns five face projections into one irradiance lives in sxuv5_reconstruct.cpp.

              The diode itself has no digital interface and firmware never touches it. The real
              hardware dependency is the ADC; the diode's parameters enter only as calibration
              constants (docs/internal/sxuv5.md §2).
  @author     Viola Case
  @date       11.08.2026
  @copyright  © Viola Case, 2026. All rights reserved.

**/

#include <pico/stdlib.h>
#include <hardware/spi.h>
#include <hardware/gpio.h>
#include <cstdint>
#include <cmath>
#include <OwlSat/sxuv5.h>
#include <OwlSat/pin_assignment.h>

namespace OwlSat {

  namespace {

    /// Boltzmann constant [J/K], for the Johnson-noise term.
    constexpr float kBoltzmann = 1.380649e-23f;

    constexpr float    kRfOhms[SXUV5_GAIN_COUNT]    = SXUV5_RF_OHMS_INIT;
    constexpr uint32_t kSettleUs[SXUV5_GAIN_COUNT]  = SXUV5_SETTLE_US_INIT;

    /// Body-frame face normals, in Face enum order. The −Z face carries the antenna.
    constexpr float kNormals[SXUV5_FACE_COUNT][3] = {
      {  1.0f,  0.0f,  0.0f }, // PlusX
      { -1.0f,  0.0f,  0.0f }, // MinusX
      {  0.0f,  1.0f,  0.0f }, // PlusY
      {  0.0f, -1.0f,  0.0f }, // MinusY
      {  0.0f,  0.0f,  1.0f }, // PlusZ
    };

    /**
     * Mux channel wired to each face. Identity for now; kept as an explicit table because the
     * harness will almost certainly not run the diodes to the mux in body-axis order.
     */
    constexpr uint8_t kMuxChannel[SXUV5_FACE_COUNT] = { 0, 1, 2, 3, 4 };

    /// Default angular response: flat cosine. Replaced by beamline calibration when it exists.
    constexpr float kAngularX[2] = { 0.0f, 1.0f };
    constexpr float kAngularY[2] = { 1.0f, 1.0f };

    /// Default temperature response: flat. Replaced by thermal-vac calibration when it exists.
    constexpr float kTempX[3] = { -20.0f, 25.0f, 80.0f };
    constexpr float kTempY[3] = { 1.0f, 1.0f, 1.0f };

    FaceCal gCal[SXUV5_FACE_COUNT];

    /// Gain that last produced an in-range code, per face. Seeds the auto-range search.
    Gain gLastGain[SXUV5_FACE_COUNT] = { Gain::Gain2, Gain::Gain2, Gain::Gain2, Gain::Gain2, Gain::Gain2 };

    bool gInitialised = false;

    float DefaultTemperature() { return SXUV5_DARK_REF_TEMP_C; }

    float (*gTemperature)() = DefaultTemperature;

    inline uint8_t Index(Face face) { return static_cast<uint8_t>(face); }
    inline uint8_t Index(Gain gain) { return static_cast<uint8_t>(gain); }


    // -----------------------------------------------------------------------
    // Built-in SPI transport
    // -----------------------------------------------------------------------

    bool SpiInit() {
      spi_init(EUV_ADC_SPI, SXUV5_SPI_BAUD);
      gpio_set_function(EUV_ADC_SCK, GPIO_FUNC_SPI);
      gpio_set_function(EUV_ADC_MOSI, GPIO_FUNC_SPI);
      gpio_set_function(EUV_ADC_MISO, GPIO_FUNC_SPI);

      // CS is driven by hand rather than by the SPI block: most SAR parts want CS to frame the
      // whole conversion, not each byte.
      gpio_init(EUV_ADC_CS);
      gpio_set_dir(EUV_ADC_CS, GPIO_OUT);
      gpio_put(EUV_ADC_CS, 1);
      return true;
    }

    /**
     * Reads one conversion frame, MSB first, and right-aligns it.
     *
     * Written against the generic "assert CS, clock out N bytes, deassert" SAR pattern. Once the
     * ADC part is chosen (docs/internal/sxuv5.md §6.4) this either stays as-is or is replaced
     * wholesale through SetAdcTransport() — which is the reason the hook exists.
     */
    bool SpiConvert(uint32_t *code_out) {
      uint8_t rx[SXUV5_ADC_FRAME_BYTES] = { 0 };
      uint8_t tx[SXUV5_ADC_FRAME_BYTES] = { 0 };

      gpio_put(EUV_ADC_CS, 0);
      const int read = spi_write_read_blocking(EUV_ADC_SPI, tx, rx, SXUV5_ADC_FRAME_BYTES);
      gpio_put(EUV_ADC_CS, 1);

      if (read != SXUV5_ADC_FRAME_BYTES) return false;

      uint32_t code = 0;
      for (int i = 0; i < SXUV5_ADC_FRAME_BYTES; ++i) code = (code << 8) | rx[i];

      code >>= SXUV5_ADC_SHIFT;
      code &= SXUV5_ADC_FULL_SCALE;

      *code_out = code;
      return true;
    }

    AdcTransport gTransport = { SpiInit, SpiConvert };


    // -----------------------------------------------------------------------
    // Mux and gain
    // -----------------------------------------------------------------------

    void InitGpio() {
      const uint pins[] = { EUV_MUX_SEL0, EUV_MUX_SEL1, EUV_MUX_SEL2, EUV_MUX_EN, EUV_TIA_GAIN0, EUV_TIA_GAIN1 };
      for (const uint pin : pins) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_OUT);
        gpio_put(pin, 0);
      }
      gpio_put(EUV_MUX_EN, 1);
    }

    void SelectFace(Face face) {
      const uint8_t channel = kMuxChannel[Index(face)];
      gpio_put(EUV_MUX_SEL0, (channel >> 0) & 1u);
      gpio_put(EUV_MUX_SEL1, (channel >> 1) & 1u);
      gpio_put(EUV_MUX_SEL2, (channel >> 2) & 1u);
    }

    void SelectGain(Gain gain) {
      const uint8_t code = Index(gain);
      gpio_put(EUV_TIA_GAIN0, (code >> 0) & 1u);
      gpio_put(EUV_TIA_GAIN1, (code >> 1) & 1u);
    }

    /// ADC code -> volts at the ADC input, honouring the signed/unsigned output format.
    float CodeToVolts(uint32_t code) {
      constexpr float kLsb = SXUV5_ADC_VREF_V / static_cast<float>(1u << SXUV5_ADC_BITS);

  #if SXUV5_ADC_SIGNED
      // Sign-extend from the ADC's width into int32_t.
      constexpr uint32_t kSignBit = 1u << (SXUV5_ADC_BITS - 1);
      int32_t            signed_code = static_cast<int32_t>(code);
      if (code & kSignBit) signed_code -= static_cast<int32_t>(1u << SXUV5_ADC_BITS);
      return static_cast<float>(signed_code) * kLsb;
  #else
      return static_cast<float>(code) * kLsb;
  #endif
    }

  } // namespace


  // =========================================================================
  // Calibration
  // =========================================================================

  float Interpolate(const Curve &c, float x) {
    if (c.n == 0 || c.x == nullptr || c.y == nullptr) return 1.0f;
    if (c.n == 1) return c.y[0];

    // Clamp rather than extrapolate: a fabricated radiometric correction is worse than a flat one.
    if (x <= c.x[0]) return c.y[0];
    if (x >= c.x[c.n - 1]) return c.y[c.n - 1];

    uint8_t hi = 1;
    while (hi < c.n - 1 && c.x[hi] < x) ++hi;

    const float x0 = c.x[hi - 1];
    const float x1 = c.x[hi];
    const float span = x1 - x0;
    if (span <= 0.0f) return c.y[hi]; // Malformed table; degrade to the upper point.

    const float t = (x - x0) / span;
    return c.y[hi - 1] + t * (c.y[hi] - c.y[hi - 1]);
  }


  FaceCal &Calibration(Face face) { return gCal[Index(face)]; }


  void ResetCalibration() {
    for (uint8_t i = 0; i < SXUV5_FACE_COUNT; ++i) {
      FaceCal &cal = gCal[i];

      for (uint8_t g = 0; g < SXUV5_GAIN_COUNT; ++g) {
        cal.rf_ohms[g]  = kRfOhms[g];
        cal.offset_v[g] = 0.0f;
      }

      cal.responsivity_a_per_w = SXUV5_RESPONSIVITY_A_PER_W;
      cal.active_area_m2       = SXUV5_ACTIVE_AREA_M2;
      cal.dark_current_a       = SXUV5_DARK_CURRENT_25C_A;
      cal.rel_uncertainty      = SXUV5_CAL_REL_UNCERTAINTY;
      cal.enabled              = true;

      cal.normal[0] = kNormals[i][0];
      cal.normal[1] = kNormals[i][1];
      cal.normal[2] = kNormals[i][2];

      cal.angular     = Curve { kAngularX, kAngularY, 2 };
      cal.temperature = Curve { kTempX, kTempY, 3 };
    }
  }


  // =========================================================================
  // Transport hooks and init
  // =========================================================================

  void SetAdcTransport(const AdcTransport &transport) {
    gTransport.init    = transport.init != nullptr ? transport.init : SpiInit;
    gTransport.convert = transport.convert != nullptr ? transport.convert : SpiConvert;
  }


  void SetTemperatureProvider(float (*provider)()) {
    gTemperature = provider != nullptr ? provider : DefaultTemperature;
  }


  bool InitEUV() {
    ResetCalibration();
    InitGpio();

    const bool ok = gTransport.init != nullptr ? gTransport.init() : true;
    gInitialised  = true;
    return ok;
  }


  // =========================================================================
  // L1 — acquisition
  // =========================================================================

  RawSample SampleRawAt(Face face, Gain gain) {
    RawSample sample {};
    sample.face  = face;
    sample.gain  = gain;
    sample.code  = 0;
    sample.flags = SXUV5_FLAG_OK;

    if (!gCal[Index(face)].enabled) {
      sample.flags |= SXUV5_FLAG_DISABLED;
      sample.timestamp_us = time_us_32();
      return sample;
    }

    SelectFace(face);
    SelectGain(gain);

    // The settle is not optional. At 100 MΩ the summing node recovers from the analog switch's
    // charge injection on a timescale comparable to the sample interval itself.
    sleep_us(kSettleUs[Index(gain)]);

    uint32_t code = 0;
    if (gTransport.convert == nullptr || !gTransport.convert(&code)) {
      sample.flags |= SXUV5_FLAG_ADC_FAULT;
      sample.timestamp_us = time_us_32();
      return sample;
    }

    sample.code         = code;
    sample.timestamp_us = time_us_32();

    const float fraction = static_cast<float>(code) / static_cast<float>(SXUV5_ADC_FULL_SCALE);
    if (fraction >= SXUV5_AUTORANGE_HIGH) sample.flags |= SXUV5_FLAG_SATURATED;
    if (fraction <= SXUV5_AUTORANGE_LOW) sample.flags |= SXUV5_FLAG_UNDERRANGE;

    return sample;
  }


  RawSample SampleRaw(Face face) {
    Gain      gain   = gLastGain[Index(face)];
    RawSample sample = SampleRawAt(face, gain);

    for (int step = 0; step < SXUV5_AUTORANGE_MAX_STEPS; ++step) {
      if (sample.flags & (SXUV5_FLAG_ADC_FAULT | SXUV5_FLAG_DISABLED)) return sample;

      const uint8_t g = Index(gain);

      if ((sample.flags & SXUV5_FLAG_SATURATED) && g > 0) {
        gain = static_cast<Gain>(g - 1); // Less transimpedance, more headroom.
      } else if ((sample.flags & SXUV5_FLAG_UNDERRANGE) && g < SXUV5_GAIN_COUNT - 1) {
        gain = static_cast<Gain>(g + 1); // More transimpedance, more resolution.
      } else {
        // Either in range, or already railed against the end of the gain ladder. A saturated
        // reading at Gain0 or an under-range reading at Gain3 is real information about the
        // signal, not a failure to converge, so the flag stands and we stop.
        gLastGain[Index(face)] = gain;
        return sample;
      }

      sample = SampleRawAt(face, gain);
    }

    // Ran out of steps while still stepping: the signal is moving, or the front end is ringing
    // (docs/internal/sxuv5.md §7). Report the last reading and say so.
    sample.flags |= SXUV5_FLAG_AUTORANGE;
    gLastGain[Index(face)] = gain;
    return sample;
  }


  ArraySample SampleEUV() {
    ArraySample pass {};
    pass.timestamp_us  = time_us_32();
    pass.temperature_c = gTemperature();

    for (uint8_t i = 0; i < SXUV5_FACE_COUNT; ++i) {
      const RawSample raw = SampleRaw(static_cast<Face>(i));
      pass.face[i] = ConvertSample(raw, pass.temperature_c);
    }

    return pass;
  }


  // =========================================================================
  // L2 — conversion
  // =========================================================================

  float RawToCurrent(const RawSample &raw, float temperature_c) {
    if (raw.flags & (SXUV5_FLAG_ADC_FAULT | SXUV5_FLAG_DISABLED)) return 0.0f;

    const FaceCal &cal = gCal[Index(raw.face)];
    const uint8_t  g   = Index(raw.gain);

    const float volts = CodeToVolts(raw.code) - cal.offset_v[g];

    const float rf = cal.rf_ohms[g];
    if (rf <= 0.0f) return 0.0f; // Malformed calibration; refuse to divide by it.

    // V_out = I_photo · R_f. The TIA inverts, but that sign is absorbed into the offset and
    // reference convention, so the magnitude relation is what firmware applies.
    const float current = volts / rf;

    // Dark current roughly doubles every SXUV5_DARK_DOUBLING_C, and at these signal levels it
    // is not a rounding error — it folds straight into the radiometric budget.
    const float delta_t = temperature_c - SXUV5_DARK_REF_TEMP_C;
    const float dark    = cal.dark_current_a * exp2f(delta_t / SXUV5_DARK_DOUBLING_C);

    return current - dark;
  }


  float CurrentToFaceIrradiance(float current_a, Face face, float temperature_c) {
    const FaceCal &cal = gCal[Index(face)];

    const float responsivity = cal.responsivity_a_per_w * Interpolate(cal.temperature, temperature_c);
    const float denominator  = responsivity * cal.active_area_m2;
    if (denominator <= 0.0f) return 0.0f;

    return current_a / denominator;
  }


  float FaceSigma(const FaceSample &sample, float temperature_c) {
    const FaceCal &cal = gCal[Index(sample.raw.face)];
    const uint8_t  g   = Index(sample.raw.gain);

    const float rf = cal.rf_ohms[g];
    if (rf <= 0.0f) return 0.0f;

    // Random terms, referred to the ADC input in volts.
    constexpr float kLsb   = SXUV5_ADC_VREF_V / static_cast<float>(1u << SXUV5_ADC_BITS);
    constexpr float kQuant = kLsb * 0.288675f; // LSB/√12

    const float johnson =
      sqrtf(4.0f * kBoltzmann * SXUV5_FRONTEND_TEMP_K * rf * SXUV5_NOISE_BANDWIDTH_HZ);

    const float sigma_v = sqrtf(kQuant * kQuant + SXUV5_ADC_NOISE_VRMS * SXUV5_ADC_NOISE_VRMS +
                                johnson * johnson);

    // Push the voltage noise through the same scale factor the signal took.
    const float sigma_i     = sigma_v / rf;
    const float sigma_rand  = fabsf(CurrentToFaceIrradiance(sigma_i, sample.raw.face, temperature_c));

    // Systematic terms scale with the reading: radiometric calibration and resistor tolerance.
    const float rel_sys   = sqrtf(cal.rel_uncertainty * cal.rel_uncertainty +
                                  SXUV5_RF_TOLERANCE * SXUV5_RF_TOLERANCE);
    const float sigma_sys = fabsf(sample.irradiance_w_m2) * rel_sys;

    return sqrtf(sigma_rand * sigma_rand + sigma_sys * sigma_sys);
  }


  FaceSample ConvertSample(const RawSample &raw, float temperature_c) {
    FaceSample sample {};
    sample.raw = raw;

    if (raw.flags & (SXUV5_FLAG_ADC_FAULT | SXUV5_FLAG_DISABLED)) {
      // No information in this channel. Leave the physical fields zeroed rather than emitting a
      // plausible-looking number that downstream code would happily average in.
      return sample;
    }

    sample.current_a       = RawToCurrent(raw, temperature_c);
    sample.irradiance_w_m2 = CurrentToFaceIrradiance(sample.current_a, raw.face, temperature_c);
    sample.sigma_w_m2      = FaceSigma(sample, temperature_c);

    if (sample.irradiance_w_m2 < SXUV5_DARK_THRESHOLD_W_M2) sample.raw.flags |= SXUV5_FLAG_DARK;

    return sample;
  }

} // namespace OwlSat
