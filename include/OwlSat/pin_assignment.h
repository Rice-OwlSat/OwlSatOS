/**

  @file       pin_assignment.h
  @brief      GPIO, bus and address assignments for every peripheral OwlSatOS drives.
  @details    All peripheral identity lives behind the macros in this file, per
              docs/internal/sxuv5.md §5, so a harness change is a one-file edit. This is the
              single pin-assignment header for the whole project: the sensor, radio, storage and
              flight-task branches all include this file rather than carrying a private copy.

              Pin numbers are placeholders until the harness drawing is released. The grouping,
              the signal names and the bus membership follow "OwlSat Hardware block diagram.png"
              and its transcription in docs/internal/hardware_block_diagram.md; the numbers do
              not. Nothing here is flight-valid until the hardware lead confirms it.

              @par What belongs here, and what does not
              Identity only — pin numbers, bus instances, device addresses. Timing, calibration
              and tuning constants live in config.h and sxuv5_config.h. The test is whether a
              value changes when the harness is redrawn: if it does, it belongs here.

  @author     Viola Case
  @date       02.09.2026
  @copyright  © Viola Case, 2026. All rights reserved.

**/
#pragma once


// ---------------------------------------------------------------------------
// Unassigned signals
//
// The block diagram names signals the harness drawing has not yet placed on a pin, and §8's
// full signal list does not fit on this package at all — see "Pin budget" at the bottom of this
// file. Those signals are defined here as OWLSAT_PIN_UNASSIGNED rather than left undefined, so
// that code referring to them still compiles and so that a driver can say honestly that its
// line does not exist yet, in the same spirit as the hal stubs.
//
// The value is negative on purpose: it is not a valid GPIO, so an unguarded gpio_init() on one
// of these is a visible fault and not a silent write to GPIO 0.
// ---------------------------------------------------------------------------

/// Sentinel for a signal the harness drawing has not yet placed. Never a valid GPIO.
#define OWLSAT_PIN_UNASSIGNED (-1)

/// True when @p pin names a real GPIO. Guard every gpio_init() of a placeholder signal with this.
#define OWLSAT_PIN_IS_ASSIGNED(pin) ((pin) >= 0)


// ---------------------------------------------------------------------------
// Legacy
// ---------------------------------------------------------------------------

/**
 * @deprecated Debug lines from the first bring-up. Not part of the EUV chain and referenced by
 * no driver on any branch. Retained until the harness drawing says what GPIO 0 and 1 are for —
 * they are the first two pins to reclaim when the budget below runs short, which it already has.
 */
#define DIODE_PIN_DATA  0
/// @deprecated See DIODE_PIN_DATA.
#define DIODE_PIN_CLOCK 1


// ---------------------------------------------------------------------------
// Shared buses
//
// I2C0 is the board-wide sensor bus: EUV ADC, sensor-domain power monitor, IMU, magnetometer,
// battery ADM1176, solar ADCs, GPS and the radio-domain power monitors all sit on it. Bus
// arbitration is therefore a board-level concern and not a per-driver one — any driver that
// talks here is sharing with the monitor reporting on its own rail.
//
// SPI0 carries the CAN controller and the magnetorquer driver, each behind its own chip select.
// Both chip selects are driven as plain GPIO rather than by the SPI block's hardware CS, which
// manages only one line and holds it for the length of a transfer rather than a transaction.
// ---------------------------------------------------------------------------

#define OWLSAT_I2C0      i2c0 ///< Board-wide sensor I2C instance.
#define OWLSAT_I2C0_SDA  4    ///< I2C0 data.
#define OWLSAT_I2C0_SCL  5    ///< I2C0 clock.

#define OWLSAT_SPI0      spi0 ///< Board-wide SPI instance: CAN controller and magnetorquer.
#define OWLSAT_SPI0_MISO 16   ///< SPI0 RX.
#define OWLSAT_SPI0_SCK  18   ///< SPI0 clock.
#define OWLSAT_SPI0_MOSI 19   ///< SPI0 TX.

#define OWLSAT_UART0     uart0 ///< GPS serial instance.
#define OWLSAT_UART0_TX  12    ///< UART0 TX — FC to GPS.
#define OWLSAT_UART0_RX  13    ///< UART0 RX — GPS to FC.


// ---------------------------------------------------------------------------
// Sensor domain — EUV science chain, per "OwlSat Hardware block diagram.png".
//
//   5x SXUV5 -> 5x coax -> MMCX -> analog mux -> variable-gain TIA -> ADS7828 -> I2C0
//
// The flight computer's only wires into the chain are the ones the diagram names:
// MUXSEL (3x), GAINSEL (2x), SENS_PWR (x2) and the shared I2C0 bus. All peripheral
// identity stays behind these macros per docs/internal/sxuv5.md §5.
// ---------------------------------------------------------------------------

#define EUV_ADC_I2C  OWLSAT_I2C0     ///< I2C instance driving the EUV ADC. Shared — see above.
#define EUV_ADC_SDA  OWLSAT_I2C0_SDA ///< I2C0 data.
#define EUV_ADC_SCL  OWLSAT_I2C0_SCL ///< I2C0 clock.

/**
 * 7-bit address of the ADS7828. A1/A0 strapping selects 0x48–0x4B; 0x48 is both pins low.
 * Must not collide with the other I2C0 occupants (ADM1176 power monitors, IMU, magnetometer).
 */
#define EUV_ADC_ADDR 0x48

#define EUV_MUX_SEL0 20 ///< MUXSEL bit 0 — analog mux channel select.
#define EUV_MUX_SEL1 21 ///< MUXSEL bit 1.
#define EUV_MUX_SEL2 22 ///< MUXSEL bit 2. Three lines only; the mux enable is strapped active.

#define EUV_TIA_GAIN0 27 ///< GAINSEL bit 0 — transimpedance gain select. Costs ADC1, see below.
#define EUV_TIA_GAIN1 28 ///< GAINSEL bit 1. Costs ADC2.

/**
 * SENS_PWR (x2) — the two channels of the dual low-power switch feeding the sensor domain.
 *
 * The science chain has no power of its own: with EUV_SENS_PWR deasserted the mux, TIA and ADC
 * are all unpowered and the ADC does not answer on I2C. The attitude channel is listed here
 * because it is the other half of the same part, but the EUV driver never drives it.
 */
#define EUV_SENS_PWR      14 ///< Science rail enable, active high.
#define ATTITUDE_SENS_PWR 15 ///< Attitude rail enable, active high. Owned by the ADCS driver.


// ---------------------------------------------------------------------------
// Radio domain
//
//   FC --SPI0/CAN_CS--> CAN controller (SPI <-> CAN) --CAN--> AMSAT LTM-1 --RF--> antennae
//
// The LTM-1 is not on the micro's buses. Everything the link layer sends reaches it through the
// controller behind CAN_CS; see include/OwlSat/can_controller.h on the radio branch. The part
// has not been selected, so CAN_INT is provisional — a polled driver would not need it, but
// every candidate part offers one and losing the pin later is cheaper than finding it later.
//
// Both radio loads are individually power-switched, so RADIO_PWR and GPS_PWR gate whether the
// controller and the GPS answer at all.
// ---------------------------------------------------------------------------

#define CAN_CS    17 ///< CAN controller chip select, active low. Software-driven.
#define CAN_INT   23 ///< CAN controller interrupt, active low. Provisional — part not chosen.
#define RADIO_PWR  6 ///< AMSAT LTM-1 power-switch enable, active high.
#define GPS_PWR    7 ///< GPS power-switch enable, active high.


// ---------------------------------------------------------------------------
// External watchdog
//
// The pulse train is generated by a task, not a timer, per the note on the drawing: a timer may
// keep running while the CPU is wedged, so a timer-driven kick proves nothing. See
// include/OwlSat/watchdog.h.
// ---------------------------------------------------------------------------

#define WDT_WDI 2 ///< Watchdog kick line, pulsed high. See OWLSAT_WATCHDOG_PERIOD_MS in config.h.

/**
 * Kick pulse width [µs].
 *
 * TBC — the external part has not been selected and its datasheet minimum is a board-level
 * number. 10 µs is comfortably above the minimum of every plausible candidate and far below the
 * kick period, so it is safe as a placeholder in both directions.
 */
#define WDT_WDI_PULSE_US 10


// ---------------------------------------------------------------------------
// Magnetorquer  (MAX22200, 4x full bridge, on SPI0 behind MAG_CS)
//
// The drawing's warning applies to whoever writes this driver: the coils must remain off unless
// they are definitely wanted on. MAG_EN is the line that enforces it, and it is deasserted at
// reset by virtue of the RP2350 bringing every GPIO up as an input.
// ---------------------------------------------------------------------------

#define MAG_CS    11 ///< MAX22200 chip select, active low. Software-driven.
#define MAG_EN     8 ///< Magnetorquer PSU enable, active high. Off unless definitely wanted on.
#define MAG_CMD    9 ///< MAX22200 command/trigger line.
#define MAG_FAULT 10 ///< Fault back from the magnetorquer, input. Whether it can also be read
                     ///< over SPI is an open question on the drawing.
#define MAG_STAT  OWLSAT_PIN_UNASSIGNED ///< PSU status back to the FC. No pin left — see below.


// ---------------------------------------------------------------------------
// Battery, heater and power monitoring
//
// Only THERM comes back to the micro as an analog signal. BAT_SENS and the two SOLAR_I_n
// currents are read by the ADM1176 and the solar ADCs respectively and reach the FC over I2C0,
// so they need no pin here — the drawing draws them green because they are analog at the
// *monitor*, not at the micro.
// ---------------------------------------------------------------------------

#define HEAT   3 ///< Heater switch enable, active high.
#define THERM 26 ///< Battery thermistor, ADC0. The only FC analog input left — see below.

#define LTC4121_1_FAULT OWLSAT_PIN_UNASSIGNED ///< FAULT+CHRG from solar charger 1. No pin left.
#define LTC4121_2_FAULT OWLSAT_PIN_UNASSIGNED ///< FAULT+CHRG from solar charger 2. No pin left.


// ---------------------------------------------------------------------------
// Antenna deployment
//
// Fired once, ever. The nonvolatile store must carry a key recording that deployment has already
// happened, so a reset cannot re-fire a burn wire — see the storage branch. Supplied from VBATT,
// not Vsolar: battery voltage is far more reliable with little solar power available.
// ---------------------------------------------------------------------------

#define BURN_0 24 ///< Burn wire 0 enable, active high.
#define BURN_1 OWLSAT_PIN_UNASSIGNED ///< No pin left — see "Pin budget" below.

/// Burn wires drawn on the diagram, whether or not each has a pin yet.
#define BURN_COUNT 2


// ---------------------------------------------------------------------------
// Debug
//
// On a Pico 2 the debug LED is the board's own, so a bench build blinks without a harness. The
// flight board's LED is a different part on the same pin number, and this macro is where that
// divergence gets recorded if the two ever separate.
// ---------------------------------------------------------------------------

#ifdef PICO_DEFAULT_LED_PIN
  #define DEBUG_LED PICO_DEFAULT_LED_PIN ///< Onboard LED on a Pico 2 (GPIO 25).
#else
  #define DEBUG_LED 25 ///< Debug LED. Matches the Pico 2's, so bench and board agree.
#endif

#define NEOPIXEL       29 ///< 4x chained WS2812 debug pixels, one data line. Costs ADC3.
#define NEOPIXEL_COUNT  4

#define USER_BUTTON OWLSAT_PIN_UNASSIGNED ///< No pin left. Boot and reset use RUN/BOOTSEL, not GPIO.


// ---------------------------------------------------------------------------
// Pin budget  —  READ THIS BEFORE ADDING A SIGNAL
//
// PICO_BOARD is pico2, so the target is an RP2350A: 30 GPIO, 0..29. Every one of them is spoken
// for above, including the two legacy debug lines on 0 and 1. The signal list in
// docs/internal/hardware_block_diagram.md §8 needs roughly thirty-five FC pins, so it does not
// fit, and the shortfall is why MAG_STAT, the two LTC4121 fault lines, BURN_1 and the user
// button are OWLSAT_PIN_UNASSIGNED rather than numbered. They are not oversights.
//
// Three ways out, in the order they are worth considering:
//
//   1. RP2350B (48 GPIO). Same die, larger package. Costs a board respin and nothing else, and
//      leaves headroom for the burn wires and fault lines the drawing has not finished drawing.
//   2. An I2C GPIO expander on I2C0. Cheap in pins and board area, and adequate for the slow
//      signals — fault lines, status, the user button. Not adequate for BURN_X, which wants a
//      line the FC can drive directly and deterministically.
//   3. Reclaim GPIO 0 and 1 from DIODE_PIN_DATA/CLOCK. Two pins, free today, and nothing on any
//      branch references them. The cheapest of the three, and the smallest.
//
// The ADC is tighter than the GPIO count suggests: of the four ADC-capable pins (26..29), 27 and
// 28 carry GAINSEL and 29 carries the neopixel data line, leaving ADC0 for THERM and nothing
// else. Any further FC analog input means moving GAINSEL down into the digital range first.
//
// Mux legality, so a reassignment does not quietly break a peripheral:
//   I2C0  SDA 0,4,8,12,16,20,24,28   SCL 1,5,9,13,17,21,25,29
//   SPI0  RX  0,4,16,20   SCK 2,6,18,22   TX 3,7,19,23
//   UART0 TX  0,12,16,28  RX  1,13,17,29
// Chip selects and every signal in the digital groups above are plain GPIO and may go anywhere.
// ---------------------------------------------------------------------------
