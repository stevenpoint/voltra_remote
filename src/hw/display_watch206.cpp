// Compiled only with -DWATCH206. CO5300 via Arduino_GFX (same as Waveshare Hello World).
#ifdef WATCH206

#include "display.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "knob.h"
#include "touch_cst816.h"

namespace {
constexpr int BUF_LINES = 40;
constexpr int TICK_PERIOD_MS = 2;

SemaphoreHandle_t s_mutex = nullptr;
lv_disp_drv_t s_disp_drv;
lv_disp_draw_buf_t s_draw_buf;
lv_indev_drv_t s_indev_drv;
lv_indev_t *s_indev = nullptr;

Arduino_DataBus *s_bus = nullptr;
Arduino_GFX *s_gfx = nullptr;

void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    const int w = area->x2 - area->x1 + 1;
    const int h = area->y2 - area->y1 + 1;
    s_gfx->draw16bitBeRGBBitmap(area->x1, area->y1, reinterpret_cast<uint16_t *>(color_p), w, h);
    lv_disp_flush_ready(drv);
}

void rounder_cb(lv_disp_drv_t *, lv_area_t *area)
{
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

void touch_cb(lv_indev_drv_t *, lv_indev_data_t *data)
{
    uint16_t x = 0, y = 0;
    const bool pressed = touch_read(x, y);
    // A swipe drives the virtual knob; drop it from LVGL so releasing it is not a click.
    if (knob_touch(pressed, y) && s_indev) lv_indev_wait_release(s_indev);
    if (pressed) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void tick_cb(void *) { lv_tick_inc(TICK_PERIOD_MS); }

void lvgl_task(void *)
{
    for (;;) {
        uint32_t delay_ms = 10;
        if (display_lock(-1)) {
            delay_ms = lv_timer_handler();
            display_unlock();
        }
        if (delay_ms > 50) delay_ms = 50;
        if (delay_ms < 2) delay_ms = 2;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}
}  // namespace

void display_set_backlight(uint8_t level)
{
    (void)level;
}

bool display_lock(int timeout_ms)
{
    if (!s_mutex) return false;
    TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_mutex, ticks) == pdTRUE;
}

void display_unlock()
{
    if (s_mutex) xSemaphoreGiveRecursive(s_mutex);
}

void display_init()
{
    s_bus = new Arduino_ESP32QSPI(PIN_LCD_CS, PIN_LCD_SCK, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3);
    s_gfx = new Arduino_CO5300(s_bus, PIN_LCD_RST, 0, LCD_H_RES, LCD_V_RES, 22, 0, 0, 0);
    if (!s_gfx->begin()) {
        log_e("CO5300 begin failed");
    }
    s_gfx->fillScreen(0x0000);

    lv_init();

    const size_t buf_px = LCD_H_RES * BUF_LINES;
    auto *buf1 = static_cast<lv_color_t *>(heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto *buf2 = static_cast<lv_color_t *>(heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf1) buf1 = static_cast<lv_color_t *>(heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL));
    if (!buf2) buf2 = static_cast<lv_color_t *>(heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL));
    assert(buf1 && buf2);
    lv_disp_draw_buf_init(&s_draw_buf, buf1, buf2, buf_px);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = LCD_H_RES;
    s_disp_drv.ver_res = LCD_V_RES;
    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.rounder_cb = rounder_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    lv_disp_t *disp = lv_disp_drv_register(&s_disp_drv);

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type = LV_INDEV_TYPE_POINTER;
    s_indev_drv.disp = disp;
    s_indev_drv.read_cb = touch_cb;
    s_indev = lv_indev_drv_register(&s_indev_drv);

    esp_timer_create_args_t tick_args = {};
    tick_args.callback = tick_cb;
    tick_args.name = "lv_tick";
    esp_timer_handle_t tick_timer = nullptr;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, TICK_PERIOD_MS * 1000));

    s_mutex = xSemaphoreCreateRecursiveMutex();
    assert(s_mutex);

    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);
    lv_refr_now(disp);
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 12 * 1024, nullptr, 2, nullptr, 1);
}

#endif
