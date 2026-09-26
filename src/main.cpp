/*
 * Voltra Remote - BLE remote for the Beyond Power Voltra I
 * Target: Waveshare ESP32-S3-Knob-Touch-LCD-1.8
 */
#include <Arduino.h>

#include "diag/flash_log.h"
#include "hw/battery.h"
#include "hw/display.h"
#include "hw/haptics.h"
#include "hw/i2c_bus.h"
#include "hw/knob.h"
#include "hw/touch_cst816.h"
#include "ui/ui.h"
#include "voltra/voltra_client.h"

void setup()
{
    Serial.begin(115200);
    delay(100);
    flash_log_begin();   // diagnostic builds only
    log_i("Voltra Remote starting (free heap %u)", (unsigned)ESP.getFreeHeap());

    i2c_bus_init();
    touch_init();
    haptics_init();
    battery_init();
    knob_init();

    display_init();

    // The client must come up before the UI: ui_init() reads the current device
    // state, and Client::begin() is what creates the mutex guarding it.
    voltra::Client::instance().begin();

    {
        LvLock lock;
        ui_init();
    }
    log_i("ready (free heap %u)", (unsigned)ESP.getFreeHeap());
}

void loop()
{
#if !VOLTRA_DIAG
    // Serial commands (screenshots, tools/screenshot.py). The diagnostic builds read
    // serial in the flash log's task instead, which passes these on.
    static char line[48];
    static size_t len = 0;
    while (Serial.available()) {
        const char ch = (char)Serial.read();
        if (ch == '\n' || ch == '\r') {
            line[len] = 0;
            if (len) ui_command(line);
            len = 0;
        } else if (len < sizeof(line) - 1) {
            line[len++] = ch;
        }
    }
#endif
    vTaskDelay(pdMS_TO_TICKS(50));
}
