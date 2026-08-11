/**

  @file       pin_assignment.h
  @brief
  @details    ~
  @author     Viola Case
  @date       17.06.2026
  @copyright  © Viola Case, 2026. All rights reserved.

**/
#pragma once

#define DIODE_PIN_DATA  0
#define DIODE_PIN_CLOCK 1


// ---------------------------------------------------------------------------
// Sensor domain — EUV science chain, per "OwlSat Hardware block diagram.png".
//
//   5x SXUV5 -> 5x coax -> MMCX -> analog mux -> variable-gain TIA -> ADS7828 -> I2C0
//
// The flight computer's only wires into the chain are the ones the diagram names:
// MUXSEL (3x), GAINSEL (2x), SENS_PWR (x2) and the shared I2C0 bus. All peripheral
// identity stays behind these macros per docs/internal/sxuv5.md §5.
// Pin numbers are placeholders until the harness drawing is released.
// ---------------------------------------------------------------------------

#define EUV_ADC_I2C  i2c0 ///< I2C instance driving the EUV ADC. Shared — see SENSOR_I2C below.
#define EUV_ADC_SDA  4    ///< I2C0 data.
#define EUV_ADC_SCL  5    ///< I2C0 clock.

/**
 * 7-bit address of the ADS7828. A1/A0 strapping selects 0x48–0x4B; 0x48 is both pins low.
 * Must not collide with the other I2C0 occupants (ADM1176 power monitors, IMU, magnetometer).
 */
#define EUV_ADC_ADDR 0x48

#define EUV_MUX_SEL0 20 ///< MUXSEL bit 0 — analog mux channel select.
#define EUV_MUX_SEL1 21 ///< MUXSEL bit 1.
#define EUV_MUX_SEL2 22 ///< MUXSEL bit 2. Three lines only; the mux enable is strapped active.

#define EUV_TIA_GAIN0 27 ///< GAINSEL bit 0 — transimpedance gain select.
#define EUV_TIA_GAIN1 28 ///< GAINSEL bit 1.

/**
 * SENS_PWR (x2) — the two channels of the dual low-power switch feeding the sensor domain.
 *
 * The science chain has no power of its own: with EUV_SENS_PWR deasserted the mux, TIA and ADC
 * are all unpowered and the ADC does not answer on I2C. The attitude channel is listed here
 * because it is the other half of the same part, but the EUV driver never drives it.
 */
#define EUV_SENS_PWR      14 ///< Science rail enable, active high.
#define ATTITUDE_SENS_PWR 15 ///< Attitude rail enable, active high. Owned by the ADCS driver.
