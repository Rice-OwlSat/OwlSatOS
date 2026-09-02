# Tutorial: the SXUV5 EUV sensor chain {#tutorial_sxuv5_euv}

**Branch:** `sxuv5-interface`.
**Reader:** someone who has to take a science reading, plug the driver into the sensor task,
bench-test it without the analog front end, load a calibration, or decide whether a number the
driver returned can be trusted.

Physics and hardware contract are in `docs/internal/sxuv5.md`; the datasheet summary is in
`SXUV5.md`. This page is the operating manual for `include/OwlSat/sxuv5.h`.

---

## 1. What the hardware is

Five SXUV5 photodiodes, one per face of the spacecraft. The sixth face, −Z, carries the antenna.
Each diode sees only the EUV projected onto its own face, so **no single diode measures the
science quantity**; the driver's last layer combines the five projections into one irradiance.

```
5x SXUV5 ─5x coax─> MMCX ─> analog mux ─> variable-gain TIA ─> ADS7828 ─I2C0─> RP2350
                            MUXSEL x3       GAINSEL x2          0x48
                            └────────── all downstream of SENS_PWR ──────────┘
```

One mux, one amplifier and one ADC serve all five diodes. That makes the array **sequential**:
every face read costs a channel change and a settling delay of up to 5 ms at the highest gain. A
full pass is on the order of 25 ms. There is no such thing as a simultaneous five-face sample.

Pins live in `include/OwlSat/pin_assignment.h` and are placeholders until the harness drawing
exists. The ADC address `0x48` must not collide with the other I2C0 occupants.

---

## 2. The four layers

Each layer is independently callable, so a failure at one does not deny access to the one below.

| Layer | Calls | Returns |
|---|---|---|
| L0 transport | `InitEUV()`, `SetEUVPower()`, `SetAdcTransport()` | rail state, bus state |
| L1 acquisition | `SampleRawAt()`, `SampleRaw()`, `SampleEUV()` | ADC codes with flags and gain |
| L2 conversion | `RawToCurrent()`, `CurrentToFaceIrradiance()`, `FaceSigma()`, `ConvertSample()` | per-face irradiance and σ |
| L3 fusion | `ScaleEUV(sample)`, `ScaleEUV(sample, sun_body)` | one true irradiance, σ, sun vector, flags |

Raw ADC codes survive to the top. They are the only part of a record that stays correct when a
calibration constant turns out to be wrong, so they are what gets downlinked.

---

## 3. Taking a science reading

The path the sensor task on `master` will use, once `Hal::UvSample()` is wired to this driver:

```cpp
#include <OwlSat/sxuv5.h>

// Once, from the task that owns the sensor:
const bool bus_ok = OwlSat::InitEUV();     // GPIO, I2C0, SENS_PWR on, default calibration

// Every cadence:
const OwlSat::ArraySample pass = OwlSat::SampleEUV();          // ~25 ms, blocking
const OwlSat::EUVResult   fit  = OwlSat::ScaleEUV(pass);       // unaided
// or, when the ADCS has a sun vector:
const OwlSat::EUVResult   fit  = OwlSat::ScaleEUV(pass, sun_body);

if (fit.flags & SXUV5_FLAG_LOWER_BOUND) {
  // fit.irradiance_w_m2 is a floor, not a measurement. Do not average it in.
}
```

Rules the caller owes the driver:

- **Cadence is yours.** The driver blocks for the pass and never delays on a timer. Pace with
  `xTaskDelayUntil()` against a fixed reference so a slow pass does not drift the schedule.
- **Serialise access.** Mux and gain state are global, so two tasks sampling different faces read
  each other's channel. And I2C0 is shared with the power monitors, IMU and magnetometer, so the
  bus lock belongs to whatever arbitrates I2C0 board-wide, not to this driver.
- **Never call from a timer callback or ISR.**
- **`InitEUV()` returning false is not fatal.** The API stays callable and every sample reports
  `SXUV5_FLAG_ADC_FAULT`, which is more information than never asking.

---

## 4. Reading the flags

Every `RawSample` carries `SXUV5Flag` bits, and every `EUVResult` carries the reconstruction-level
ones. Read them before the numbers.

**Per-face, "this channel measured something":**

| Flag | Meaning | Number is |
|---|---|---|
| `SATURATED` | code ≥ 90 % of full scale even at the lowest gain | a floor |
| `UNDERRANGE` | code ≤ 6 % of full scale even at the highest gain | noisy but real |
| `DARK` | below `SXUV5_DARK_THRESHOLD_W_M2` | face is not sunlit |
| `AUTORANGE` | gain search hit its step limit | last reading; signal moving or front end ringing |
| `GRAZING` | face read light where geometry says it cannot see the sun | excluded from fit; suspect attitude or normal |
| `OUTLIER` | inconsistent with the other faces | dropped from the fit |

**Per-face, "this channel carries no information"** (`SXUV5_FLAGS_NO_DATA`):

| Flag | Meaning |
|---|---|
| `ADC_FAULT` | I2C transfer failed or timed out |
| `DISABLED` | `Calibration(face).enabled == false` |
| `UNPOWERED` | `SENS_PWR` is off — a power decision, not a fault |

**Reconstruction-level, in `EUVResult::flags`:**

| Flag | Meaning | What to do |
|---|---|---|
| `ECLIPSE` | no face is sunlit | irradiance is not measurable; do not trend it |
| `Z_UNOBSERVED` | sun is in the −Z hemisphere and only the unaided solver ran | see `LOWER_BOUND` |
| `LOWER_BOUND` | result is a floor on the true irradiance | **branch on this**; roughly half the sky triggers it |
| `DEGENERATE` | too few usable faces | no solution |
| `OUT_OF_FAMILY` | outside ×10 of the nominal solar level | suspect the chain, not the sun |
| `POOR_FIT` | faces disagree beyond their stated σ | attitude or calibration is off |

`SXUV5_FLAG_LOWER_BOUND` is the important one. The unaided solver has no −Z diode, so whenever the
sun is behind the antenna face it can only bound the Z component. This is the common case, not a
corner case, and it is the reason to pass a sun vector whenever attitude knowledge exists.

---

## 5. Which `ScaleEUV()` to call

| | `ScaleEUV(sample)` | `ScaleEUV(sample, sun_body)` |
|---|---|---|
| Needs | nothing | unit sun vector in body frame from the ADCS |
| Coverage | half the sky gives a lower bound | whole sky except a narrow cone about −Z |
| Noise | one face's word per axis | weighted average across every sunlit face |
| Consistency check | none (`chi_square` = 0, `dof` = 0) | χ² and a one-shot outlier rejection |
| Detects a drifting diode | no | yes, as `OUTLIER` then `POOR_FIT` |

Pass the sun vector when you have it. A zero-length or null vector falls back to the unaided
solve automatically, so the call site does not need a branch.

`GetEUVUncertainty(result)` returns `sigma_w_m2`. It is computed during the fit and cannot be
reconstructed afterwards, so read it from the result rather than recomputing.

---

## 6. Bench-testing without the analog front end

The ADC is the only thing firmware talks to, and it can be replaced:

```cpp
static bool FakeInit() { return true; }
static bool FakeConvert(uint32_t *code) { *code = 2048; return true; }   // mid-scale

OwlSat::SetAdcTransport({ FakeInit, FakeConvert });
OwlSat::InitEUV();
```

`convert` is called with the rail up, the mux channel selected and the gain settled, and must
return a right-aligned code. Return false to inject an `ADC_FAULT`. Pass `nullptr` members to
fall back to the built-in ADS7828 driver.

To exercise the reconstruction with realistic geometry, make `FakeConvert` consult a global "sun
vector" and return `E_nominal · max(0, n_face · s)` converted back to a code at the current gain.
The face and gain in force are not passed to `convert`, so track them by hooking `SampleRawAt()`
call order, or drive `SampleRawAt(face, gain)` directly and skip the auto-range.

Temperature correction defaults to a constant that makes the corrections no-ops. To wire it:

```cpp
OwlSat::SetTemperatureProvider(ReadTiaThermistorC);   // NOT the RP2350 die sensor
```

---

## 7. Power

The whole chain is downstream of one channel of the `SENS_PWR` dual switch. It is the
subsystem's only low-power state.

```cpp
OwlSat::SetEUVPower(false);   // parks select lines low first, then drops the rail
// ... later ...
OwlSat::SetEUVPower(true);    // absorbs SXUV5_SENS_PWR_SETTLE_US (20 ms) before returning
```

While the rail is down every sample reports `UNPOWERED`, not `ADC_FAULT`. `EUVPowered()` returns
the commanded state, not a measured one; the ADM1176 on that rail is what can confirm current.

`SENS_PWR` is also the recovery mechanism for a wedged ADS7828 holding SDA low, which an I2C
master cannot otherwise clear. Power-cycle the chain before declaring the bus dead.

---

## 8. Calibration

Defaults come from `include/OwlSat/sxuv5_config.h`. Anything marked **TBC** there is a
dimensionally-correct placeholder, not a flight value, and the build prints a `#pragma message`
saying so until `SXUV5_CONSTANTS_CONFIRMED` is defined.

Runtime calibration is per face and mutable, so ground can push revised numbers without a
software load:

```cpp
OwlSat::FaceCal &cal = OwlSat::Calibration(OwlSat::Face::PlusX);
cal.rf_ohms[3]          = 0.98e9f;     // as-measured, top gain step
cal.responsivity_a_per_w = 0.27f * 0.95f;  // with 5 % degradation folded in
cal.enabled              = false;      // mark a dead diode; fusion drops it
```

`Curve` members (`angular`, `temperature`) point at caller-owned tables that must outlive the
curve. Interpolation clamps at the ends rather than extrapolating. `ResetCalibration()` restores
the compile-time defaults for every face.

Things that must be confirmed by the hardware lead before any number is trusted (full list in
`docs/internal/sxuv5.md` §6): feedback resistances, bias mode, mux channel map (`kMuxChannel[]`
is identity until the harness says otherwise), SENS_PWR settling time, and TIA compensation.

**If readings are noisy, bimodal or out of family in a way that scales with signal, the front end
is ringing.** That is an analog compensation problem and cannot be filtered in software. Confirm a
compensation capacitor exists before writing any averaging logic.

---

## 9. Plugging into the task layer on `master`

`src/hal_stub.cpp` on master has the merge recipe in its `MERGE:` comment. The shape:

```cpp
bool Hal::UvInit()  { return OwlSat::InitEUV(); }

bool Hal::UvSample(UvSample *out) {
  const ArraySample pass = OwlSat::SampleEUV();
  const EUVResult   fit  = OwlSat::ScaleEUV(pass);        // or with sun_body
  if (fit.method == ReconstructMethod::None && !(fit.flags & SXUV5_FLAG_ECLIPSE)) return false;
  for (i in faces) { out->code[i] = pass.face[i].raw.code; out->gain[i] = ...; out->face_flags[i] = ...; }
  out->irradiance_w_m2 = fit.irradiance_w_m2;  out->sigma_w_m2 = fit.sigma_w_m2;
  out->flags = fit.flags;  out->face_mask = fit.face_mask;  out->timestamp_us = pass.timestamp_us;
  return true;
}
```

Two things to keep straight at merge time:

- `OWLSAT_UV_FACE_COUNT` in `config.h` and `SXUV5_FACE_COUNT` here are declared separately and
  must agree. Add a `static_assert`.
- A pass in which some faces faulted is still a success; the failed faces carry their flags. Only
  return false when there is no usable pass at all.

`CMakeLists.txt` on this branch adds `hardware_i2c` to the link line. Master does not have it.

---

## 10. Where to go next

- `docs/tutorials/flight_tasks.md` (branch `flight-tasks` / `master`) — the sensor task this feeds.
- `docs/internal/sxuv5.md` — hardware contract, blocking unknowns, stability constraint.
- `include/OwlSat/sxuv5.h` — the maths behind both solvers, in the function comments.
