#include "i2c_bus.h"
#include "board_pins.h"

static SemaphoreHandle_t s_mutex = nullptr;

void i2c_bus_init()
{
    if (s_mutex) return;
    s_mutex = xSemaphoreCreateRecursiveMutex();
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
    Wire.setTimeOut(20);
}

I2cLock::I2cLock()
{
    if (s_mutex) xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
}

I2cLock::~I2cLock()
{
    if (s_mutex) xSemaphoreGiveRecursive(s_mutex);
}
