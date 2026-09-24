#include "display.h"

#include <Arduino.h>

#include <assert.h>

#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "lcd_qspi_panel.h"
#include "touch_cst816.h"

namespace {
constexpr int BUF_LINES = 40;                // 2 x 360 x 40 x 2 B = 57.6 KB of DMA RAM
constexpr int TICK_PERIOD_MS = 2;
constexpr spi_host_device_t LCD_HOST = SPI2_HOST;

SemaphoreHandle_t s_mutex = nullptr;
esp_lcd_panel_handle_t s_panel = nullptr;
lv_disp_drv_t s_disp_drv;
lv_disp_draw_buf_t s_draw_buf;
lv_indev_drv_t s_indev_drv;

bool on_color_trans_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *user_ctx)
{
    lv_disp_flush_ready(static_cast<lv_disp_drv_t *>(user_ctx));
    return false;
}

void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    (void)drv;
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_p);
}

/* The panel wants even-aligned windows. */
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
    if (touch_read(x, y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void tick_cb(void *)
{
    lv_tick_inc(TICK_PERIOD_MS);
}

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
    ledcWrite(PIN_LCD_BL, level);
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
    // Backlight off while the panel initialises
    ledcAttach(PIN_LCD_BL, 50000, 8);
    ledcWrite(PIN_LCD_BL, 0);

    spi_bus_config_t bus = {};
    bus.data0_io_num = PIN_LCD_D0;
    bus.data1_io_num = PIN_LCD_D1;
    bus.sclk_io_num = PIN_LCD_SCK;
    bus.data2_io_num = PIN_LCD_D2;
    bus.data3_io_num = PIN_LCD_D3;
    bus.max_transfer_sz = LCD_H_RES * BUF_LINES * sizeof(lv_color_t);
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num = PIN_LCD_CS;
    io_cfg.dc_gpio_num = -1;
    io_cfg.spi_mode = 0;
    io_cfg.pclk_hz = 40 * 1000 * 1000;
    io_cfg.trans_queue_depth = 10;
    io_cfg.on_color_trans_done = on_color_trans_done;
    io_cfg.user_ctx = &s_disp_drv;
    io_cfg.lcd_cmd_bits = 32;
    io_cfg.lcd_param_bits = 8;
    io_cfg.flags.quad_mode = 1;
    esp_lcd_panel_io_handle_t io_handle = nullptr;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io_handle));

    ESP_ERROR_CHECK(lcd_qspi_panel_create(io_handle, PIN_LCD_RST, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    lv_init();

    const size_t buf_px = LCD_H_RES * BUF_LINES;
    auto *buf1 = static_cast<lv_color_t *>(heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    auto *buf2 = static_cast<lv_color_t *>(heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    assert(buf1 && buf2);
    lv_disp_draw_buf_init(&s_draw_buf, buf1, buf2, buf_px);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = LCD_H_RES;
    s_disp_drv.ver_res = LCD_V_RES;
    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.rounder_cb = rounder_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.user_data = s_panel;
    lv_disp_t *disp = lv_disp_drv_register(&s_disp_drv);

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type = LV_INDEV_TYPE_POINTER;
    s_indev_drv.disp = disp;
    s_indev_drv.read_cb = touch_cb;
    lv_indev_drv_register(&s_indev_drv);

    esp_timer_create_args_t tick_args = {};
    tick_args.callback = tick_cb;
    tick_args.name = "lv_tick";
    esp_timer_handle_t tick_timer = nullptr;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, TICK_PERIOD_MS * 1000));

    s_mutex = xSemaphoreCreateRecursiveMutex();
    assert(s_mutex);

    // Clear to black before the backlight comes on
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);
    lv_refr_now(disp);

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 12 * 1024, nullptr, 2, nullptr, 1);
    ledcWrite(PIN_LCD_BL, 255);
}
