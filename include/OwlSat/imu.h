/**

  @file       imu.h
  @brief      Attitude-sensor read interface: accelerometer + magnetometer over I2C0.
  @details    This is one driver in a family — the board carries up to three attitude-sensor
              parts (a 6 DOF gyro+accel IMU, candidates LSM6DSOTR or BMI088; a 3 DOF magnetometer,
              LSM303AGRTR; per docs/internal/hardware_block_diagram.md — plus whichever third part
              this file was written against). The struct and function names here are generic on
              purpose so every such driver can expose the same shape; which chip actually answers
              on the bus is an implementation detail of the .cpp, called out at the top of it.

              @par What this file currently backs
              Right now Init()/ReadAccel()/ReadMag() are implemented against the LSM303AGR, which
              provides the accelerometer and magnetometer blocks but no gyroscope — see the .cpp
              for the datasheet references. There is deliberately no ReadGyro() here: that would
              mean fabricating angular-rate data for hardware this file is not talking to. When
              the gyro+accel IMU part is chosen and its datasheet is in hand, it gets its own
              driver; whether that becomes a second file with its own Init/Read functions or a
              second backend behind this same interface is a decision for when that part is known,
              not before.

              @par Bus ownership
              This is presently the only I2C0 driver in the tree, so Init() also brings up the
              I2C0 peripheral itself (pins, baud rate) and owns the mutex that serialises every
              transaction. Per pin_assignment.h, I2C0 is shared board-wide with the EUV ADC, the
              power monitors and GPS. When a second I2C0 driver lands, the bus bring-up and the
              mutex need to move to something both drivers share rather than being duplicated
              per-driver — see the note beside i2c_init() in the .cpp.

              @par Threading
              Every function here blocks on a FreeRTOS mutex with a bounded timeout and must be
              called from task context only — never from an ISR — matching OwlSat::Hal's contract
              even though this driver does not sit behind Hal.

  @author     Viola Case
  @date       11.09.2026
  @copyright  © Viola Case, 2026. All rights reserved.

**/
#pragma once

#include <cstdint>

namespace OwlSat::Imu {

  /// One accelerometer sample. Normal mode (10-bit), FS = ±2 g.
  struct AccelSample {
    float    x_g;
    float    y_g;
    float    z_g;
    uint32_t timestamp_ms;
  };

  /// One magnetometer sample. High-resolution continuous mode, FS = ±50 gauss.
  struct MagSample {
    float    x_gauss;
    float    y_gauss;
    float    z_gauss;
    uint32_t timestamp_ms;
  };

  /**
   * @brief Brings up I2C0, confirms both WHO_AM_I registers, and configures both sensor blocks.
   *
   * Safe to call once at startup. ReadAccel() and ReadMag() refuse to run until this has
   * succeeded, so a failed Init() fails loud rather than quietly returning zeros later.
   *
   * @return False if either device did not answer, did not identify correctly, or a
   *         configuration write failed.
   */
  bool Init();

  /**
   * @brief Reads one accelerometer sample, if the device has new data.
   * @param out Filled on success; untouched on failure.
   * @return False if new data was not ready, the transfer failed, or Init() has not succeeded.
   */
  bool ReadAccel(AccelSample *out);

  /**
   * @brief Reads one magnetometer sample, if the device has new data.
   * @param out Filled on success; untouched on failure.
   * @return False if new data was not ready, the transfer failed, or Init() has not succeeded.
   */
  bool ReadMag(MagSample *out);

} // namespace OwlSat::Imu
