#include "knob.h"

#ifdef WATCH206
// No encoder on the watch: vertical swipes on the touchscreen stand in for it.
#include <atomic>
#include <stdlib.h>

namespace {
constexpr int DRAG_START_PX = 12;   // movement before a touch counts as a swipe, not a tap
constexpr int DETENT_PX = 24;       // finger travel per virtual detent

std::atomic<int> s_delta{0};
bool s_down = false;
bool s_dragging = false;
int s_anchor_y = 0;
}  // namespace

void knob_init() {}

bool knob_touch(bool pressed, int y)
{
    if (!pressed) {
        s_down = false;
        s_dragging = false;
        return false;
    }
    if (!s_down) {
        s_down = true;
        s_anchor_y = y;
        return false;
    }
    if (!s_dragging) {
        if (abs(y - s_anchor_y) < DRAG_START_PX) return false;
        s_dragging = true;
        s_anchor_y = y;
    }
    // Swipe up = clockwise (value up).
    const int detents = (s_anchor_y - y) / DETENT_PX;
    if (detents) {
        s_delta += detents;
        s_anchor_y -= detents * DETENT_PX;
    }
    return true;
}

int knob_take_delta() { return s_delta.exchange(0); }
#else

#include <Arduino.h>
#include <atomic>

#include "driver/gpio.h"
#include "esp_timer.h"

#include "board_pins.h"

#ifndef KNOB_QUADRATURE
#define KNOB_QUADRATURE 0
#endif
#ifndef KNOB_INVERT
#define KNOB_INVERT 0
#endif

namespace {
constexpr uint32_t TICK_US = 3000;     // poll period, same as the Waveshare demo
constexpr uint8_t DEBOUNCE_TICKS = 2;

std::atomic<int> s_delta{0};
esp_timer_handle_t s_timer = nullptr;

struct Channel {
    uint8_t prev = 1;
    uint8_t debounce = 0;
};
Channel s_a, s_b;

/* Port of process_knob_channel() from the Waveshare bidi_switch_knob.c demo:
 * fires once per rising edge that follows at least one stable low sample. */
bool rising_edge(Channel &ch, uint8_t level)
{
    bool fired = false;
    if (level == 0) {
        if (level != ch.prev) ch.debounce = 0; else ch.debounce++;
    } else {
        if (level != ch.prev && ++ch.debounce >= DEBOUNCE_TICKS) {
            ch.debounce = 0;
            fired = true;
        } else {
            ch.debounce = 0;
        }
    }
    ch.prev = level;
    return fired;
}

void poll_cb(void *)
{
    uint8_t a = gpio_get_level((gpio_num_t)PIN_ENC_A);
    uint8_t b = gpio_get_level((gpio_num_t)PIN_ENC_B);
#if KNOB_QUADRATURE
    if (rising_edge(s_a, a)) {
        s_delta += (b == 0) ? 1 : -1;
    }
    rising_edge(s_b, b);
#else
    if (rising_edge(s_a, a)) s_delta += 1;
    if (rising_edge(s_b, b)) s_delta -= 1;
#endif
}
}  // namespace

void knob_init()
{
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << PIN_ENC_A) | (1ULL << PIN_ENC_B);
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);

    s_a.prev = gpio_get_level((gpio_num_t)PIN_ENC_A);
    s_b.prev = gpio_get_level((gpio_num_t)PIN_ENC_B);

    esp_timer_create_args_t args = {};
    args.callback = poll_cb;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "knob";
    esp_timer_create(&args, &s_timer);
    esp_timer_start_periodic(s_timer, TICK_US);
}

int knob_take_delta()
{
    int d = s_delta.exchange(0);
#if KNOB_INVERT
    d = -d;
#endif
    return d;
}
#endif
