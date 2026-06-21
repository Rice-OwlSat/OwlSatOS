/**

  @file       sxuv5.h
  @brief      
  @details    ~
  @author     Viola Case
  @date       15.06.2026
  @copyright  © Viola Case, 2026. All rights reserved.
  
**/
#pragma once

namespace OwlSat {

  using SXUV5_t = float;

  const SXUV5_t SampleEUV();

  /**
   * @brief Scales raw EUV sample to get the actual data desired
   * @param sample Raw EUV diode data sample
   * @return EUV value scaled and refined properly in currently unknown units
   */
  float ScaleEUV(SXUV5_t sample);

  /**
   *
   * @param sample Raw EUV diode data sample
   * @return Uncertainty in the same units as `ScaleEUV()`
   */
  float GetEUVUncertainty(SXUV5_t sample);

}

