# SXUV5 EUV Photodiode

**Role:** Primary science sensor — solar EUV irradiance.\
**Type:** Stabilized silicon photodiode (Opto Diode SXUV series), responsive ~1–120 nm.
Engineered surface passivation resists EUV-induced responsivity degradation; stability
approaches NIST transfer-standard grade.

## Detector physics
- Current source: I_ph = R(λ) · Φ, where Φ is incident optical power [W].
- In the EUV/soft-X-ray band each absorbed photon liberates E_photon / w electron–hole
  pairs, with w ≈ 3.63 eV (Si pair-creation energy). Internal gain is therefore >1 and
  responsivity is flat at ≈ q/w ≈ 0.27 A/W, wavelength-independent — except for dips at
  absorption edges (Si L-edge ≈ 99.8 eV / 12.4 nm; any window/oxide K-edges).
- Solar EUV onto a ~5 mm² die → sub-µW incident → low-nA photocurrent (order 1–50 nA).
- Run photovoltaic (zero bias) into a virtual ground: minimal dark current and shot noise,
  ideal for a faint near-DC signal. Reverse bias only buys bandwidth you don't need here.

## Front end (transimpedance amp)
- I→V conversion: V_out = −I_ph · R_f. For 10 nA → 1 V, R_f = 100 MΩ.
- That large R_f dictates the rest:
    - Op-amp input bias must be fA-class (electrometer/FET input) so I_bias ≪ I_ph.
    - Noise is set by Johnson noise of R_f (∝ √R_f) and op-amp e_n scaled by the noise gain
      (1 + C_in/C_f). Guard the summing node; board leakage rivals the signal at 100 MΩ.
    - C_f across R_f compensates (C_diode + C_in); f_−3dB = 1/(2π R_f C_f). 100 MΩ ∥ 0.5 pF
      ⇒ ~3 kHz, far above a near-DC signal.
- Dark current ~doubles per 8–10 °C; offset and drift fold straight into the radiometric error.

## Digitization
Semultiplexer/analog switch, a variable-gain transimpedance amp (2 GPIOs on the RP2350 to select the gain), and an external ADC chip


