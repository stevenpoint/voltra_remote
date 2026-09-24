#pragma once
// Waveshare ESP32-S3-Knob-Touch-LCD-1.8 — ESP32-S3 side pin map
// (GPIOs taken from the Waveshare demo code, which is authoritative over the schematic.)

// ST77916 360x360 LCD over QSPI
#define PIN_LCD_CS      14
#define PIN_LCD_SCK     13
#define PIN_LCD_D0      15
#define PIN_LCD_D1      16
#define PIN_LCD_D2      17
#define PIN_LCD_D3      18
#define PIN_LCD_RST     21
#define PIN_LCD_BL      47   // backlight, PWM

#define LCD_H_RES       360
#define LCD_V_RES       360

// Shared I2C bus: CST816 touch (0x15) + DRV2605 haptics (0x5A)
#define PIN_I2C_SDA     11
#define PIN_I2C_SCL     12
#define PIN_TOUCH_INT   9
#define PIN_TOUCH_RST   10
#define I2C_ADDR_CST816  0x15
#define I2C_ADDR_DRV2605 0x5A

// Rotary knob (primary encoder on the ESP32-S3)
#define PIN_ENC_A       8
#define PIN_ENC_B       7

// Battery sense: ADC1_CH0 through a 10k/10k divider
#define PIN_BAT_ADC     1
