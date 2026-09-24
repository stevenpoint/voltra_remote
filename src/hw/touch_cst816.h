#pragma once
#include <stdint.h>

/** Initialise the CST816 touch controller on the shared I2C bus (Wire). */
bool touch_init();

/** Read the current touch point. Returns true while a finger is down. */
bool touch_read(uint16_t &x, uint16_t &y);
