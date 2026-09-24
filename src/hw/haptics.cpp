#include "haptics.h"

#include <Arduino.h>
#include <Wire.h>

#include "board_pins.h"
#include "i2c_bus.h"

namespace {
constexpr uint8_t REG_STATUS      = 0x00;
constexpr uint8_t REG_MODE        = 0x01;
constexpr uint8_t REG_RTPIN       = 0x02;
constexpr uint8_t REG_LIBRARY     = 0x03;
constexpr uint8_t REG_WAVESEQ1    = 0x04;
constexpr uint8_t REG_WAVESEQ2    = 0x05;
constexpr uint8_t REG_GO          = 0x0C;
constexpr uint8_t REG_OVERDRIVE   = 0x0D;
constexpr uint8_t REG_SUSTAINPOS  = 0x0E;
constexpr uint8_t REG_SUSTAINNEG  = 0x0F;
constexpr uint8_t REG_BREAK       = 0x10;
constexpr uint8_t REG_AUDIOMAX    = 0x13;
constexpr uint8_t REG_FEEDBACK    = 0x1A;
constexpr uint8_t REG_CONTROL3    = 0x1D;

constexpr uint8_t MODE_INTTRIG = 0x00;

bool s_ok = false;

bool write_reg(uint8_t reg, uint8_t val)
{
    Wire.beginTransmission(I2C_ADDR_DRV2605);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

bool read_reg(uint8_t reg, uint8_t &val)
{
    Wire.beginTransmission(I2C_ADDR_DRV2605);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((uint8_t)I2C_ADDR_DRV2605, (uint8_t)1) != 1) return false;
    val = Wire.read();
    return true;
}
}  // namespace

bool haptics_init()
{
    I2cLock lock;
    uint8_t status = 0;
    if (!read_reg(REG_STATUS, status)) {
        log_w("DRV2605 not found");
        s_ok = false;
        return false;
    }
    // Same bring-up as the Waveshare demo / Adafruit driver: ERM open loop, library 1.
    write_reg(REG_MODE, MODE_INTTRIG);
    write_reg(REG_RTPIN, 0x00);
    write_reg(REG_WAVESEQ1, 1);
    write_reg(REG_WAVESEQ2, 0);
    write_reg(REG_OVERDRIVE, 0);
    write_reg(REG_SUSTAINPOS, 0);
    write_reg(REG_SUSTAINNEG, 0);
    write_reg(REG_BREAK, 0);
    write_reg(REG_AUDIOMAX, 0x64);
    uint8_t fb = 0;
    if (read_reg(REG_FEEDBACK, fb)) write_reg(REG_FEEDBACK, fb & 0x7F);
    uint8_t c3 = 0;
    if (read_reg(REG_CONTROL3, c3)) write_reg(REG_CONTROL3, c3 | 0x20);
    write_reg(REG_LIBRARY, 1);
    s_ok = true;
    log_i("DRV2605 ready (status 0x%02x)", status);
    return true;
}

void haptics_play(uint8_t effect)
{
    if (!s_ok) return;
    I2cLock lock;
    write_reg(REG_WAVESEQ1, effect);
    write_reg(REG_WAVESEQ2, 0);
    write_reg(REG_GO, 1);
}

void haptics_tick()   { haptics_play(26); }  // sharp tick 3 - 60%
void haptics_click()  { haptics_play(1);  }  // strong click 100%
void haptics_double() { haptics_play(10); }  // double click 100%
void haptics_buzz()   { haptics_play(47); }  // buzz 1 - 100%
