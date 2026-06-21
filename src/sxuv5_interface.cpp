/**

  @file       sxuv5_interface.cpp
  @brief
  @details    ~
  @author     Viola Case
  @date       15.06.2026
  @copyright  © Viola Case, 2026. All rights reserved.

**/

#include <pico/stdlib.h>
#include <cstdint>
#include <OwlSat/sxuv5.h>
#include <cstring>

// Assuming 8 bit parallel, will change accordingly later

// These are all dummy numbers
#define DIODE_PIN_1 5
#define DIODE_PIN_2 6
#define DIODE_PIN_3 7
#define DIODE_PIN_4 8
#define DIODE_PIN_5 9
#define DIODE_PIN_6 10
#define DIODE_PIN_7 11
#define DIODE_PIN_8 12

namespace OwlSat {

  /**
   *
   * @return
   */
  const SXUV5_t SampleEUV() {
    const uint8_t byte = (gpio_get(DIODE_PIN_1) << 7) + (gpio_get(DIODE_PIN_2) << 6) + (gpio_get(DIODE_PIN_3) << 5) +
                         (gpio_get(DIODE_PIN_4) << 4) + (gpio_get(DIODE_PIN_5) << 3) + (gpio_get(DIODE_PIN_6) << 2) +
                         (gpio_get(DIODE_PIN_7) << 1) + (gpio_get(DIODE_PIN_8));

    SXUV5_t result;
    memcpy(&result, &byte, sizeof(byte));

    return result;
  }


  float ScaleEUV(SXUV5_t sample) {
    // Define EUV scale function

    return sample;
  }


  float GetEUVUncertainty(SXUV5_t sample) {
    // Do the calculation of uncertainty based on contents of ScaleEUV function

    return sample;
  }

} // namespace OwlSat
