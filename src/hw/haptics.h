#pragma once
#include <stdint.h>

/** DRV2605 haptic driver on the knob. All calls are no-ops if init failed. */
bool haptics_init();
void haptics_play(uint8_t effect);   // DRV2605 library-1 effect id (1..123)
void haptics_tick();                 // light detent tick for dial steps
void haptics_click();                // firm click for button taps
void haptics_double();               // double click for load/unload confirmation
void haptics_buzz();                 // short buzz for errors
