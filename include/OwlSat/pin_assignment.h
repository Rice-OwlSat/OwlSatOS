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
// EUV chain: SXUV5 ×5 -> analog mux -> variable-gain TIA -> external ADC -> SPI
//
// All peripheral identity stays behind these macros per docs/internal/sxuv5.md §5.
// Pin numbers are placeholders until the harness drawing is released.
// ---------------------------------------------------------------------------

#define EUV_ADC_SPI  spi0 ///< SPI instance driving the EUV ADC.
#define EUV_ADC_SCK  18   ///< SPI clock.
#define EUV_ADC_MOSI 19   ///< SPI master-out (command/config frames).
#define EUV_ADC_MISO 16   ///< SPI master-in (conversion result).
#define EUV_ADC_CS   17   ///< Per-device chip select — gives the fault isolation I2C cannot.

#define EUV_MUX_SEL0 20 ///< Analog mux channel select, bit 0.
#define EUV_MUX_SEL1 21 ///< Analog mux channel select, bit 1.
#define EUV_MUX_SEL2 22 ///< Analog mux channel select, bit 2.
#define EUV_MUX_EN   26 ///< Analog mux enable, active high. Set to -1 if the part is always enabled.

#define EUV_TIA_GAIN0 27 ///< Transimpedance gain select, bit 0.
#define EUV_TIA_GAIN1 28 ///< Transimpedance gain select, bit 1.
