#pragma once
#include <stdint.h>

void battery_init();
uint32_t battery_millivolts();   // pack voltage in mV (0 if no battery)
int battery_percent();           // rough LiPo state of charge, -1 if unknown
