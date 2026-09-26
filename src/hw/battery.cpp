#include "battery.h"

#ifdef WATCH206
// Watch 2.06: the battery sits behind an AXP2101 PMIC on the shared I2C bus.
#include <Arduino.h>
#include "esp_sleep.h"
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

#include "board_pins.h"
#include "i2c_bus.h"

namespace {
XPowersPMU s_pmu;
bool s_ok = false;

int pmu_read(uint8_t addr, uint8_t reg, uint8_t *data, uint8_t len)
{
    I2cLock lock;
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return -1;
    if (Wire.requestFrom(addr, len) != len) return -1;
    for (uint8_t i = 0; i < len; i++) data[i] = Wire.read();
    return 0;
}

int pmu_write(uint8_t addr, uint8_t reg, uint8_t *data, uint8_t len)
{
    I2cLock lock;
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(data, len);
    return Wire.endTransmission() == 0 ? 0 : -1;
}
}  // namespace

void battery_init()
{
    i2c_bus_init();
    s_ok = s_pmu.begin(I2C_ADDR_AXP2101, pmu_read, pmu_write);
    if (!s_ok) {
        log_e("AXP2101 not found at 0x%02x", I2C_ADDR_AXP2101);
        return;
    }
    // Same setup as Waveshare's 01_AXP2101 example. The board has no battery thermistor:
    // with the TS pin check left on, the PMIC sees a temperature fault and charging
    // misbehaves.
    s_pmu.disableTSPinMeasure();
    s_pmu.enableBattDetection();
    s_pmu.enableBattVoltageMeasure();
    s_pmu.enableVbusVoltageMeasure();
    s_pmu.enableSystemVoltageMeasure();
    s_pmu.enableTemperatureMeasure();
    s_pmu.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_50MA);
    s_pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_400MA);
    s_pmu.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);
    s_pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
}

uint32_t battery_millivolts()
{
    return s_ok ? s_pmu.getBattVoltage() : 0;   // 0 when no battery is detected
}

int battery_percent()
{
    return s_ok ? s_pmu.getBatteryPercent() : -1;   // PMIC fuel gauge, -1 without a battery
}

bool on_external_power()
{
    return (s_ok && s_pmu.isVbusIn()) || HWCDC::isPlugged();
}

void power_off()
{
    if (s_ok) s_pmu.shutdown();
    // No PMIC: sleep with no wake source, so only a reset brings it back.
    esp_deep_sleep_start();
}

#else

#include <Arduino.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"

#include "board_pins.h"

void battery_init()
{
    analogReadResolution(12);
    // Arduino 3.x only accepts a per-pin attenuation once the channel exists, so
    // take a throwaway reading first. Without this it logs
    // "Pin is not configured as analog channel" and keeps the default attenuation.
    (void)analogRead(PIN_BAT_ADC);
    analogSetPinAttenuation(PIN_BAT_ADC, ADC_11db);
}

uint32_t battery_millivolts()
{
    uint32_t acc = 0;
    for (int i = 0; i < 8; i++) {
        acc += analogReadMilliVolts(PIN_BAT_ADC);
    }
    return (acc / 8) * 2;   // 10k/10k divider
}

int battery_percent()
{
    uint32_t mv = battery_millivolts();
    if (mv < 3000) return -1;       // no battery attached / running from USB
    // Simple piecewise LiPo curve.
    struct Pt { uint32_t mv; int pct; };
    static const Pt curve[] = {
        {4200, 100}, {4100, 90}, {4000, 78}, {3900, 62}, {3800, 45},
        {3700, 25}, {3600, 12}, {3500, 5}, {3300, 0},
    };
    if (mv >= curve[0].mv) return 100;
    for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); i++) {
        if (mv >= curve[i].mv) {
            const Pt &a = curve[i - 1];
            const Pt &b = curve[i];
            return b.pct + (int)((mv - b.mv) * (a.pct - b.pct) / (a.mv - b.mv));
        }
    }
    return 0;
}

bool on_external_power()
{
    // No power sensing on the knob's USB port: a computer at the other end is what counts.
    return HWCDC::isPlugged();
}

void power_off()
{
    // Backlight off, and held off through sleep: GPIO 47 is not an RTC pin, so it would
    // otherwise float. display_init() releases the hold on the next boot.
    ledcWrite(PIN_LCD_BL, 0);
    ledcDetach(PIN_LCD_BL);
    pinMode(PIN_LCD_BL, OUTPUT);
    digitalWrite(PIN_LCD_BL, LOW);
    gpio_hold_en((gpio_num_t)PIN_LCD_BL);
    gpio_deep_sleep_hold_en();

    // Wake on a touch (the touch controller pulls its interrupt low) or a turn of the
    // knob (either encoder line pulsing low). An encoder line already resting low would
    // wake it at once, so it is left out.
    uint64_t mask = 1ULL << PIN_TOUCH_INT;
    if (gpio_get_level((gpio_num_t)PIN_ENC_A)) mask |= 1ULL << PIN_ENC_A;
    if (gpio_get_level((gpio_num_t)PIN_ENC_B)) mask |= 1ULL << PIN_ENC_B;
    for (int pin = 0; pin < 64; pin++) {
        if (!(mask & (1ULL << pin))) continue;
        rtc_gpio_pullup_en((gpio_num_t)pin);
        rtc_gpio_pulldown_dis((gpio_num_t)pin);
    }
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);   // keep the pull-ups
    esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
}
#endif
