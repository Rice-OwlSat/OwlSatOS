/**
 * @file imu.cpp
 * @brief Backs OwlSat::Imu with the ST LSM303AGR accelerometer + magnetometer.
 *
 * This is the LSM303AGR-specific half of the generic interface declared in imu.h. If a second
 * attitude-sensor part (the 6 DOF gyro+accel IMU, or a third part) needs its own driver, it reads
 * and writes different registers than the ones below, but should be reachable through the same
 * Init/ReadAccel/ReadMag shape.
 *
 * Register map, timing and startup sequence are from the LSM303AGR datasheet (STMicroelectronics
 * DocID027765 Rev 11, Aug 2022). Section/table references below point back to it so a register
 * value can be checked without re-deriving it.
 */

#include <cstdio>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include <hardware/i2c.h>
#include <pico/stdlib.h>

#include <OwlSat/imu.h>
#include <OwlSat/pin_assignment.h>

namespace OwlSat::Imu {

  namespace {

    // -------------------------------------------------------------------
    // I2C bus addresses  (§6.1.1 "Default address", Table 24, Table 25)
    // -------------------------------------------------------------------

    constexpr uint8_t kAccelAddr = 0x19; ///< 0011001b, accelerometer 7-bit slave address.
    constexpr uint8_t kMagAddr   = 0x1E; ///< 0011110b, magnetometer 7-bit slave address.

    /// SUB-address auto-increment bit. Set on a multi-byte transfer so the device walks the
    /// register address forward itself (§6.1.1: "If the MSb of the SUB field is 1, the SUB
    /// (register address) is automatically increased to allow multiple data read/writes").
    constexpr uint8_t kAutoIncrement = 0x80;

    /// I2C fast mode, the faster of the two speeds the interface is specified against (Table 7).
    constexpr uint32_t kI2cBaudHz = 400000;

    // -------------------------------------------------------------------
    // Accelerometer registers  (Table 26: register 00h-3Fh block)
    // -------------------------------------------------------------------

    constexpr uint8_t kRegWhoAmIAccel = 0x0F;
    constexpr uint8_t kRegCtrl1Accel  = 0x20;
    constexpr uint8_t kRegCtrl4Accel  = 0x23;
    constexpr uint8_t kRegStatusAccel = 0x27;
    constexpr uint8_t kRegOutXLAccel  = 0x28; ///< First of 6 bytes: X_L X_H Y_L Y_H Z_L Z_H.

    constexpr uint8_t kWhoAmIAccelExpected = 0x33; ///< Table 30.

    /// BDU=1, FS=±2g, HR=0 (Table 41/42). BDU stops a read from tearing an L/H pair across two
    /// updates; the datasheet's own startup example (§5.3) skips it, but Table 42 documents the
    /// hazard it exists to avoid.
    constexpr uint8_t kCtrl4AccelValue = 0x80;

    /// ODR=0101 (100 Hz), LPen=0, Zen=Yen=Xen=1 -> normal mode, all axes on (Table 33/35,
    /// matches the "Accel = 100 Hz (normal mode)" line of §5.3's startup sequence).
    constexpr uint8_t kCtrl1AccelValue = 0x57;

    /// Normal mode (LPen=0, HR=0), FS=±2g: 3.9 mg/LSB (Table 3, LA_So). Output is left-justified
    /// two's complement (§8.14-8.16), hence the >>6 in ReadAccel() before this is applied.
    constexpr float kAccelSensitivityG = 0.0039f;

    // -------------------------------------------------------------------
    // Magnetometer registers  (Table 26: register 40h-6Fh block)
    // -------------------------------------------------------------------

    constexpr uint8_t kRegWhoAmIMag = 0x4F;
    constexpr uint8_t kRegCfgAMag   = 0x60;
    constexpr uint8_t kRegCfgCMag   = 0x62;
    constexpr uint8_t kRegStatusMag = 0x67;
    constexpr uint8_t kRegOutXLMag  = 0x68; ///< First of 6 bytes: X_L X_H Y_L Y_H Z_L Z_H.

    constexpr uint8_t kWhoAmIMagExpected = 0x40; ///< §8.38.

    /// BDU=1, everything else default (Table 98/99). §5.3's example uses CFG_REG_C_M=01h to
    /// drive DRDY as a digital output for an interrupt-driven read; this driver polls
    /// STATUS_REG_M instead (task context only, no ISR — see the header), so that bit is left
    /// clear and BDU is set in its place.
    constexpr uint8_t kCfgCMagValue = 0x10;

    /// COMP_TEMP_EN=1, LP=0, ODR=00 (10 Hz), MD=00 (continuous) (Table 91-94). §5.3's example
    /// writes 00h here, but Table 92's footnote on COMP_TEMP_EN says "for proper operation, this
    /// bit must be set to 1" — that footnote, not the worked example, is what this follows.
    constexpr uint8_t kCfgAMagValue = 0x80;

    /// 1.5 mgauss/LSB (Table 3, M_So). Magnetometer output is plain 16-bit two's complement, NOT
    /// left-justified like the accelerometer's (§8.47-8.49 vs §8.14-8.16) — no shift needed.
    constexpr float kMagSensitivityGauss = 0.0015f;

    // -------------------------------------------------------------------
    // Shared status bit  (Table 51 STATUS_REG_A, Table 109 STATUS_REG_M — same layout)
    // -------------------------------------------------------------------

    constexpr uint8_t kStatusZyxda = 0x08; ///< bit3: a full X/Y/Z set is ready to read.

    // -------------------------------------------------------------------
    // Bus state
    // -------------------------------------------------------------------

    SemaphoreHandle_t g_bus_mutex   = nullptr;
    bool              g_initialized = false;

    bool TakeBus() {
      return g_bus_mutex != nullptr && xSemaphoreTake(g_bus_mutex, pdMS_TO_TICKS(50)) == pdTRUE;
    }

    void GiveBus() {
      xSemaphoreGive(g_bus_mutex);
    }

    bool WriteReg(uint8_t addr, uint8_t reg, uint8_t value) {
      const uint8_t payload[2] = {reg, value};
      return i2c_write_blocking(OWLSAT_I2C0, addr, payload, sizeof(payload), false)
             == static_cast<int>(sizeof(payload));
    }

    /// Multi-byte reads set kAutoIncrement and use a repeated START before the read, per the
    /// "SAD+W, SUB, SR, SAD+R" pattern in Table 22/23 and the "SR ... if the bit is 1 (read)"
    /// rule in §6.1.1 — i2c_write_blocking's nostop=true is that repeated START.
    bool ReadRegs(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len) {
      const uint8_t sub = (len > 1) ? static_cast<uint8_t>(reg | kAutoIncrement) : reg;
      if (i2c_write_blocking(OWLSAT_I2C0, addr, &sub, 1, true) != 1) {
        return false;
      }
      return i2c_read_blocking(OWLSAT_I2C0, addr, buf, len, false) == static_cast<int>(len);
    }

  } // namespace

  bool Init() {
    if (g_bus_mutex == nullptr) {
      g_bus_mutex = xSemaphoreCreateMutex();
      if (g_bus_mutex == nullptr) {
        return false;
      }
    }

    // First (and currently only) I2C0 driver on this branch, so it owns bus bring-up. If a
    // second I2C0 driver (EUV ADC, GPS, power monitors) lands, this call and g_bus_mutex both
    // need to become shared rather than duplicated per-driver — see pin_assignment.h's note on
    // I2C0 arbitration being a board-level concern.
    i2c_init(OWLSAT_I2C0, kI2cBaudHz);
    gpio_set_function(OWLSAT_I2C0_SDA, GPIO_FUNC_I2C);
    gpio_set_function(OWLSAT_I2C0_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(OWLSAT_I2C0_SDA);
    gpio_pull_up(OWLSAT_I2C0_SCL);

    if (!TakeBus()) {
      printf("[imu] Init: could not take the I2C0 mutex\n");
      return false;
    }

    uint8_t who = 0;
    bool ok = ReadRegs(kAccelAddr, kRegWhoAmIAccel, &who, 1) && who == kWhoAmIAccelExpected;
    if (!ok) {
      printf("[imu] Init: accelerometer WHO_AM_I mismatch or no answer (got 0x%02X)\n", who);
    }

    if (ok) {
      ok = ReadRegs(kMagAddr, kRegWhoAmIMag, &who, 1) && who == kWhoAmIMagExpected;
      if (!ok) {
        printf("[imu] Init: magnetometer WHO_AM_I mismatch or no answer (got 0x%02X)\n", who);
      }
    }

    ok = ok && WriteReg(kAccelAddr, kRegCtrl4Accel, kCtrl4AccelValue);
    ok = ok && WriteReg(kAccelAddr, kRegCtrl1Accel, kCtrl1AccelValue);
    ok = ok && WriteReg(kMagAddr, kRegCfgCMag, kCfgCMagValue);
    ok = ok && WriteReg(kMagAddr, kRegCfgAMag, kCfgAMagValue);

    GiveBus();

    if (!ok) {
      printf("[imu] Init: configuration failed; sensor stays uninitialised\n");
      return false;
    }

    // Turn-on margin: magnetometer high-resolution turn-on is up to 9.4 ms (Table 12); the
    // accelerometer's normal-mode turn-on is shorter (Table 14). 20 ms covers both with margin.
    vTaskDelay(pdMS_TO_TICKS(20));

    g_initialized = true;
    return true;
  }

  bool ReadAccel(AccelSample *out) {
    if (out == nullptr || !g_initialized || !TakeBus()) {
      return false;
    }

    uint8_t status = 0;
    uint8_t raw[6] = {0};
    bool ok = ReadRegs(kAccelAddr, kRegStatusAccel, &status, 1);
    ok = ok && (status & kStatusZyxda) != 0;
    ok = ok && ReadRegs(kAccelAddr, kRegOutXLAccel, raw, sizeof(raw));

    GiveBus();
    if (!ok) {
      return false;
    }

    // Left-justified two's complement (§8.14-8.16): the 10-bit normal-mode value sits in the top
    // 10 bits of the 16-bit pair, so >>6 recovers it. The shift is on a value already widened to
    // int by integer promotion, so it sign-extends correctly for a negative reading.
    const int16_t x = static_cast<int16_t>((raw[1] << 8) | raw[0]) >> 6;
    const int16_t y = static_cast<int16_t>((raw[3] << 8) | raw[2]) >> 6;
    const int16_t z = static_cast<int16_t>((raw[5] << 8) | raw[4]) >> 6;

    out->x_g         = x * kAccelSensitivityG;
    out->y_g         = y * kAccelSensitivityG;
    out->z_g         = z * kAccelSensitivityG;
    out->timestamp_ms = static_cast<uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount()));
    return true;
  }

  bool ReadMag(MagSample *out) {
    if (out == nullptr || !g_initialized || !TakeBus()) {
      return false;
    }

    uint8_t status = 0;
    uint8_t raw[6] = {0};
    bool ok = ReadRegs(kMagAddr, kRegStatusMag, &status, 1);
    ok = ok && (status & kStatusZyxda) != 0;
    ok = ok && ReadRegs(kMagAddr, kRegOutXLMag, raw, sizeof(raw));

    GiveBus();
    if (!ok) {
      return false;
    }

    // Plain 16-bit two's complement, not left-justified (§8.47-8.49) — no shift, unlike the
    // accelerometer above.
    const int16_t x = static_cast<int16_t>((raw[1] << 8) | raw[0]);
    const int16_t y = static_cast<int16_t>((raw[3] << 8) | raw[2]);
    const int16_t z = static_cast<int16_t>((raw[5] << 8) | raw[4]);

    out->x_gauss      = x * kMagSensitivityGauss;
    out->y_gauss      = y * kMagSensitivityGauss;
    out->z_gauss      = z * kMagSensitivityGauss;
    out->timestamp_ms = static_cast<uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount()));
    return true;
  }

} // namespace OwlSat::Imu
