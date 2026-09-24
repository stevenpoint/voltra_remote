#pragma once
#include <lvgl.h>

/** Bring up the QSPI panel, LVGL, touch input and the LVGL task. */
void display_init();

/** Serialise LVGL API calls from other tasks. */
bool display_lock(int timeout_ms = -1);
void display_unlock();

/** Backlight 0..255 */
void display_set_backlight(uint8_t level);

/** RAII helper */
class LvLock {
public:
    LvLock() { display_lock(-1); }
    ~LvLock() { display_unlock(); }
    LvLock(const LvLock &) = delete;
    LvLock &operator=(const LvLock &) = delete;
};
