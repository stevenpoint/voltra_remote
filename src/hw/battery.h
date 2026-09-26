#pragma once
#include <stdint.h>

void battery_init();
uint32_t battery_millivolts();   // pack voltage in mV (0 if no battery)
int battery_percent();           // rough LiPo state of charge, -1 if unknown

/** Plugged in: USB power on the watch (charger or computer); a computer on the knob. */
bool on_external_power();

/**
 * Switch the device off. The watch's PMIC cuts power (the side button turns it back on);
 * the knob goes into deep sleep and wakes, restarting, on a touch or a turn of the knob.
 * Does not return.
 */
void power_off();
