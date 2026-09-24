#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/** Bring up the shared I2C bus (touch + haptics). Safe to call more than once. */
void i2c_bus_init();

/** RAII guard serialising access to Wire between tasks. */
class I2cLock {
public:
    I2cLock();
    ~I2cLock();
    I2cLock(const I2cLock &) = delete;
    I2cLock &operator=(const I2cLock &) = delete;
};
