#include "battery.h"

#include <Arduino.h>

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
