# SXUV5 EUV Photodiode — Hardware Integration Spec

**Target:** OwlSatOS / RP2350B / FreeRTOS
**Purpose:** Everything the firmware layer needs to know about the SXUV5 to write and validate a driver. This is a contract document, not an implementation.

---

## 1. Device Summary

| Property | Value | Source |
|---|---|---|
| Part | Opto Diode SXUV5 (ODD-SXU-008) | Datasheet rev. 2024-10-10 |
| Type | Silicon photodiode, radiation-hard EUV | |
| Active area | 5 mm² circular, Ø2.5 mm | |
| Spectral range | 1–190 nm | |
| Package | TO-5 metal can, windowless, 3 leads (TO-39-3) | |
| Shunt resistance R_sh | 20 MΩ min | |
| Junction capacitance C_j | 500 pF typ / **1500 pF max** @ V_R = 0 | |
| Response time | 1 µs typ / 2 µs max @ R_L = 50 Ω, V_R = 0 | |
| Reverse breakdown | 5 V min / 20 V typ | |
| Operating temp (vacuum) | −20 °C to +80 °C | |
| Operating temp (ambient) | −10 °C to +40 °C | |

The windowless package is why this part was selected: any window material is opaque below roughly 150 nm and would eliminate most of the band of interest.

---

## 2. Signal Chain

```
5x SXUV5 ──(photocurrent)──> 5x coax ──> MMCX ──> analog mux ──> variable-gain TIA ──(voltage)──> ADS7828 ──(I2C0)──> RP2350B
```

Per the hardware block diagram (`docs/internal/hardware_block_diagram.md`, Sensor Domain), one multiplexer and one amplifier serve all five diodes, and the whole chain sits downstream of one channel of a dual low-power switch driven by `SENS_PWR`.

**Consequence for the driver:** the array is sequential, not parallel. Every reading costs a mux change and the settling time that follows it, and there is no such thing as a simultaneous five-face sample. Expected photocurrent from a sunlit face is ~1.3 nA (block diagram, from the LISIRD TIMED SEE SSI instrument), which is what sizes the gain ladder.

The photodiode has **no digital interface**. Firmware never touches the diode. The driver's actual hardware dependency is the ADC; the diode's parameters enter only as calibration constants and as constraints on achievable noise, range, and bandwidth.

**Consequence for the driver:** the SXUV5 driver is a thin conversion layer sitting on top of an ADC driver. It owns unit conversion and channel identity. It does not own bus transactions.

---

## 3. Conversion Math

The TIA converts photocurrent to voltage:

```
V_out = I_photo × R_f
```

Firmware inverts this:

```
I_photo = (code / 2^N) × V_ref / R_f
```

Where `N` is ADC resolution and `V_ref` is the ADC reference. Optical power follows from responsivity, which is strongly wavelength-dependent across 1–190 nm:

```
P_optical = I_photo / R(λ)
```

`R(λ)` is a curve, not a scalar. The driver returns **current**, not irradiance. Spectral interpretation is a ground-segment problem, not a flight-software problem — do not embed a responsivity table in firmware.

---

## 4. Driver Interface Contract

```cpp
namespace owlsat::sxuv5 {

// Raw, unconverted ADC code. No scaling, no calibration.
uint32_t read_raw();

// Photocurrent in amps. Applies V_ref, resolution, and R_f.
float read_current();

}  // namespace owlsat::sxuv5
```

Rules:

- Both calls are **synchronous and blocking** on the SPI transaction. They do not delay, retry on a timer, or manage cadence.
- Cadence lives in the FreeRTOS task layer via `vTaskDelayUntil` against a fixed reference timestamp. Sample rate and publish rate remain independently tunable.
- `read_raw` must remain available in flight. When calibration constants drift or prove wrong, raw codes are the only recoverable record.
- Conversion constants (`R_f`, `V_ref`, ADC resolution) are compile-time configuration, not literals scattered through the conversion function.

---

## 5. Bus and Pin Abstraction

Per project convention, all peripheral identity goes behind macros:

```
EUV_ADC_I2C
EUV_ADC_SDA
EUV_ADC_SCL
EUV_ADC_ADDR
EUV_MUX_SEL0..2      (MUXSEL, 3x)
EUV_TIA_GAIN0..1     (GAINSEL, 2x)
EUV_SENS_PWR         (SENS_PWR, science channel)
```

**This section previously argued for SPI, and the hardware went the other way.** The argument was sound and is worth keeping on record: per-device chip select gives fault isolation, and a hung EUV ADC on SPI cannot wedge the bus for every other peripheral, which an I2C slave holding SDA low absolutely can. The block diagram puts the ADS7828 on the shared **I2C0** segment anyway, alongside the power monitors, the IMU, the magnetometer and the solar ADCs.

What the driver owes in exchange for losing that isolation:

- Every transfer is bounded by `SXUV5_I2C_TIMEOUT_US`, never allowed to block indefinitely. A part that stops acknowledging becomes a flagged sample and releases the bus.
- The driver does not own the I2C0 lock. Bus arbitration is board-wide, because the ADC shares the segment with the monitor reporting on its own rail.
- `SENS_PWR` is a recovery mechanism as well as a power-budget one: a wedged ADS7828 can be power-cycled, which is the one thing an I2C master cannot do about a slave holding SDA low.

---

## 6. Blocking Unknowns

Driver cannot be completed until the hardware lead confirms:

1. **R_f (TIA feedback resistance)** — sets the amps-per-volt scale factor. Without it, `read_current` cannot be written.
2. **Bias mode** — zero-bias (photovoltaic) or reverse-biased (photoconductive). Zero-bias gives lower dark current and better long-term stability; reverse bias reduces C_j and increases bandwidth at the cost of dark current. For multi-second integration cadence, zero-bias is the expected choice. Confirm, don't assume.
3. **Case lead disposition** — the third TO-5 lead. Grounded to the can for shielding, brought out as a guard ring, or floating. Affects noise floor and grounding topology.
4. ~~**ADC part number, resolution, and V_ref**~~ — **answered by the block diagram:** ADS7828, 8-channel 12-bit, internal 2.5 V reference, I2C. Listed as "ADC options: ADS7828 (from AMSAT)", so it is a strong intent rather than a signed-off selection; the transport hook in `sxuv5.h` remains the escape route if it changes. What it fixes: LSB = 610 µV, full scale = 2.5 V, and the code path in `sxuv5_interface.cpp`.
5. **TIA compensation** — see §7.
6. **Mux part and channel map** — the diagram draws three select lines and no enable, so the part is assumed 8:1 with its enable strapped active. `kMuxChannel[]` in `sxuv5_interface.cpp` is identity until the harness drawing says otherwise, and it almost certainly will.
7. **SENS_PWR settling time** — how long after the switch closes the ADS7828's reference and the TIA are both trustworthy. Budgeted at 20 ms in `sxuv5_config.h`; measure it.

---

## 7. Stability Constraint (read before trusting any measurement)

C_j is up to **1500 pF**. That capacitance sits across the TIA's inverting input and forms a pole with R_f. At the high feedback resistances needed for small EUV photocurrents, this pole lands low enough to cause peaking or sustained oscillation unless a compensation capacitor is placed across R_f.

Firmware symptom: readings that are noisy, bimodal, or wildly out of family in a way that scales with signal level. This is not a software bug and cannot be filtered away in software. If the analog front end rings, the driver is reporting the ringing faithfully.

Confirm a compensation cap exists before writing any averaging or filtering logic, or that logic will be tuned against an artifact.

---

## 8. Note on Response Time

Several vendor and distributor pages state 1–2 **ns** response time. The manufacturer datasheet (rev. 2024-10-10) states 1–2 **µs**. The datasheet governs.

Either figure is orders of magnitude faster than the mission's sampling cadence, so this does not constrain the driver. It is documented here so nobody re-derives the discrepancy later and assumes the datasheet is wrong.

---

## 9. Redundancy

The mission architecture carries multiple EUV sensors. The driver must therefore be **instance-based, not singleton**: channel identity is a construction parameter, not a global. A failed sensor must degrade to a flagged bad channel, not a failed subsystem.
