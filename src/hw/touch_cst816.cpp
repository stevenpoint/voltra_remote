#include "touch_cst816.h"

#include <Arduino.h>
#include <Wire.h>

#include "board_pins.h"
#include "i2c_bus.h"

namespace {
constexpr uint8_t REG_FINGERNUM = 0x02;
constexpr uint8_t REG_CHIP_ID   = 0xA7;
constexpr uint8_t REG_DIS_AUTOSLEEP = 0xFE;

bool write_reg(uint8_t reg, uint8_t val)
{
    Wire.beginTransmission(I2C_ADDR_CST816);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

bool read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    Wire.beginTransmission(I2C_ADDR_CST816);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    size_t got = Wire.requestFrom((uint8_t)I2C_ADDR_CST816, (uint8_t)len);
    if (got != len) {
        while (Wire.available()) Wire.read();
        return false;
    }
    for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
    return true;
}
}  // namespace

bool touch_init()
{
    pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
    pinMode(PIN_TOUCH_RST, OUTPUT);
    digitalWrite(PIN_TOUCH_RST, LOW);
    delay(10);
    digitalWrite(PIN_TOUCH_RST, HIGH);
    delay(80);

    I2cLock lock;
    uint8_t id = 0;
    bool ok = read_regs(REG_CHIP_ID, &id, 1);
    // Keep the controller awake so the first tap after idle is not swallowed.
    write_reg(REG_DIS_AUTOSLEEP, 0x01);
    log_i("CST816 chip id 0x%02x (%s)", id, ok ? "ok" : "no ack");
    return ok;
}

bool touch_read(uint16_t &x, uint16_t &y)
{
    uint8_t d[7] = {0};
    {
        I2cLock lock;
        if (!read_regs(0x00, d, sizeof(d))) {
            return false;
        }
    }
    if (d[REG_FINGERNUM] == 0) {
        return false;
    }
    x = ((uint16_t)(d[3] & 0x0F) << 8) | d[4];
    y = ((uint16_t)(d[5] & 0x0F) << 8) | d[6];
    if (x >= LCD_H_RES) x = LCD_H_RES - 1;
    if (y >= LCD_V_RES) y = LCD_V_RES - 1;
    return true;
}
