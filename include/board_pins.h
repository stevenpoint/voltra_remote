#pragma once

#ifdef WATCH206
// Waveshare ESP32-S3-Touch-AMOLED-2.06
#define PIN_LCD_CS      12
#define PIN_LCD_SCK     11
#define PIN_LCD_D0       4
#define PIN_LCD_D1       5
#define PIN_LCD_D2       6
#define PIN_LCD_D3       7
#define PIN_LCD_RST      8
#define PIN_LCD_BL      -1
#define PIN_LCD_TE      13

#define LCD_H_RES       410
#define LCD_V_RES       502

#define PIN_I2C_SDA     15
#define PIN_I2C_SCL     14
#define PIN_TOUCH_INT   38
#define PIN_TOUCH_RST    9
#define I2C_ADDR_CST816  0x38
#define I2C_ADDR_FT3168  0x38
#define I2C_ADDR_DRV2605 0x5A
#define I2C_ADDR_AXP2101 0x34

#define PIN_ENC_A       -1
#define PIN_ENC_B       -1
#define PIN_BAT_ADC     -1

#else
// Waveshare ESP32-S3-Knob-Touch-LCD-1.8
#define PIN_LCD_CS      14
#define PIN_LCD_SCK     13
#define PIN_LCD_D0      15
#define PIN_LCD_D1      16
#define PIN_LCD_D2      17
#define PIN_LCD_D3      18
#define PIN_LCD_RST     21
#define PIN_LCD_BL      47

#define LCD_H_RES       360
#define LCD_V_RES       360

#define PIN_I2C_SDA     11
#define PIN_I2C_SCL     12
#define PIN_TOUCH_INT   9
#define PIN_TOUCH_RST   10
#define I2C_ADDR_CST816  0x15
#define I2C_ADDR_DRV2605 0x5A

#define PIN_ENC_A       8
#define PIN_ENC_B       7
#define PIN_BAT_ADC     1
#endif
