#pragma once
/*
 * Drop sets, as in marvaments/voltra-knob-controller: lower the weight by a percent of
 * itself, either after every so many reps of a set (rep target) or each time a set ends
 * (set down), up to a number of drops per load. Only the weight drops; chains and
 * eccentric stay as set.
 *
 * Header-only and free of Arduino/LVGL dependencies so it can be unit-tested on
 * the host.
 */
#include <stdint.h>

namespace dropsets {

enum Mode : uint8_t { OFF, REP_TARGET, SET_DOWN, MODE_COUNT };

/** What the Voltra on screen reports, as the drop decision needs it. */
struct Sample {
    bool loaded;       // the weight is on
    bool set_active;   // a set is under way (status active and the cable has moved)
    bool resting;      // the Voltra's workout status is resting
    int reps;          // the Voltra's rep count
};

/** How far the current load has got. Feed it every state change of the Voltra on screen. */
struct Tracker {
    int done = 0;              // drops made since this load started
    bool was_loaded = false;
    bool was_active = false;
    int last_reps = -1;        // rep count last seen, -1 none yet this load

    /** Start over, e.g. on another Voltra: the next sample counts as a new load. */
    void reset()
    {
        was_loaded = was_active = false;
        last_reps = -1;
    }

    /**
     * @return true when a drop is due now (and counts it). A new load starts the drops
     * over. Rep target drops when a new rep brings the count to the next multiple of
     * `every`; set down drops when a set ends with the Voltra resting, not unloading.
     */
    bool step(Mode mode, int max_drops, int every, const Sample &s)
    {
        if (s.loaded && !was_loaded) {
            done = 0;
            last_reps = s.reps;
        }
        was_loaded = s.loaded;
        const bool set_ended = was_active && !s.set_active;
        was_active = s.set_active;
        const bool new_rep = s.reps != last_reps;
        last_reps = s.reps;

        if (!s.loaded || mode == OFF || done >= max_drops) return false;
        bool due = false;
        if (mode == REP_TARGET) due = s.set_active && new_rep && every > 0 && s.reps >= every * (done + 1);
        else if (mode == SET_DOWN) due = set_ended && s.resting;
        if (due) done++;
        return due;
    }
};

/** The weight after one drop of `pct` percent, rounded to the nearest pound. */
inline int dropped_weight(int lb, int pct) { return (lb * (100 - pct) + 50) / 100; }

}  // namespace dropsets
