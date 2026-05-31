#include "lvgl_port.h"
#include "lcd_st7789.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_check.h"
#include "esp_timer.h"
#include "lvgl.h"

#define LVGL_TICK_MS 2

static SemaphoreHandle_t s_mutex = NULL;

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_MS);
}

static void lvgl_task(void *arg)
{
    while (1) {
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGive(s_mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void lvgl_port_init(void)
{
    lv_init();
    lcd_st7789_init();

    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t buf1[LCD_H_RES * 40];
    static lv_color_t buf2[LCD_H_RES * 40];

    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LCD_H_RES * 40);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &draw_buf;

    lcd_st7789_attach_lvgl(&disp_drv);
    lv_disp_drv_register(&disp_drv);

    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };

    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_MS * 1000));

    s_mutex = xSemaphoreCreateMutex();
}

void lvgl_port_start(void)
{
    xTaskCreate(lvgl_task, "lvgl_task", 4096, NULL, 5, NULL);
}

void lvgl_port_lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

void lvgl_port_unlock(void)
{
    xSemaphoreGive(s_mutex);
}

void lvgl_port_refresh_now(void)
{
    lv_timer_handler();
    lv_refr_now(NULL);
}
