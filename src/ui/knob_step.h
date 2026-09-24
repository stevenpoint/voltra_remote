#pragma once
/*
 * Knob stepping maths: how far one detent moves a value, based on how fast the
 * knob is being turned.
 *
 * A deliberate, slow turn moves 1 lb per detent so you can dial an exact weight.
 * Spinning the knob switches to 5 lb per detent, so crossing the full 5-200 lb
 * range stays a short travel.
 *
 * Header-only and free of Arduino/LVGL dependencies so it can be unit-tested on
 * the host.
 */
#include <stdint.h>

namespace knobstep {

constexpr int STEP_FINE_LB = 1;
constexpr int STEP_COARSE_LB = 5;

/** Per-detent interval thresholds, with a dead band so the mode cannot flap mid-turn. */
constexpr uint32_t COARSE_ENTER_MS = 70;   // faster than this -> coarse
constexpr uint32_t COARSE_EXIT_MS = 140;   // slower than this -> fine
constexpr uint32_t IDLE_RESET_MS = 400;    // a pause this long resets to fine

/** Division rounding towards negative / positive infinity (C truncates towards zero). */
inline int floor_div(int a, int b)
{
    int q = a / b, r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) q--;
    return q;
}

inline int ceil_div(int a, int b)
{
    int q = a / b, r = a % b;
    if (r != 0 && ((r < 0) == (b < 0))) q++;
    return q;
}

/**
 * Move `value` by `detents` steps of `step`.
 *
 * Coarse steps snap onto multiples of `step` in the direction of travel, so speeding
 * up from 47 lb lands on 50 rather than 52 and the number on screen stays round.
 * Fine steps (step <= 1) are a plain add, so exact values are reachable.
 */
inline int apply_step(int value, int detents, int step)
{
    if (step <= 1) return value + detents;
    int base = (detents > 0) ? floor_div(value, step) : ceil_div(value, step);
    return (base + detents) * step;
}

/** Tracks knob turn rate and reports the step size to use for each batch of detents. */
class RateTracker {
public:
    /**
     * @param detents signed detent count seen since the last call (must not be 0)
     * @param now_ms  current millisecond clock
     * @return step size in lb for this batch
     */
    int step(int detents, uint32_t now_ms)
    {
        const int n = detents < 0 ? -detents : detents;
        const uint32_t elapsed = now_ms - last_ms_;
        if (!started_ || elapsed >= IDLE_RESET_MS) {
            // First movement after a pause is always fine, so a single deliberate
            // click is 1 lb no matter how fast the previous turn was.
            coarse_ = false;
        } else if (n > 0) {
            const uint32_t per_detent = elapsed / (uint32_t)n;
            if (per_detent <= COARSE_ENTER_MS) coarse_ = true;
            else if (per_detent >= COARSE_EXIT_MS) coarse_ = false;
        }
        last_ms_ = now_ms;
        started_ = true;
        return coarse_ ? STEP_COARSE_LB : STEP_FINE_LB;
    }

    bool coarse() const { return coarse_; }

    void reset()
    {
        coarse_ = false;
        started_ = false;
        last_ms_ = 0;
    }

private:
    bool coarse_ = false;
    bool started_ = false;
    uint32_t last_ms_ = 0;
};

}  // namespace knobstep
