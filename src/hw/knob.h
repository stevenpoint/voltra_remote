#pragma once
#include <stdint.h>

/**
 * Rotary knob input.
 *
 * The Waveshare demo treats the encoder as a "bidirectional switch": a pulse on
 * phase A means one step clockwise, a pulse on phase B one step counter-clockwise.
 * That is the default here. Define KNOB_QUADRATURE=1 if your unit turns out to be a
 * conventional quadrature encoder (direction taken from B at each A rising edge).
 * Define KNOB_INVERT=1 to flip the direction.
 */
void knob_init();

/** Steps accumulated since the last call (positive = clockwise). */
int knob_take_delta();

#ifdef WATCH206
/**
 * Watch 2.06 has no encoder, so vertical swipes act as the knob. Feed every touch
 * sample here; returns true once the touch has become a swipe, in which case the
 * caller should stop it from also counting as a tap.
 */
bool knob_touch(bool pressed, int y);
#endif
