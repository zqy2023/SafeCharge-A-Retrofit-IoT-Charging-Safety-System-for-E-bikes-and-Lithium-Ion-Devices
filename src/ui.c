#include "ui.h"

#include <stdbool.h>
#include <stdio.h>

#include "dht11.h"
#include "ina219.h"
#include "smoke_sensor.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lvgl_port.h"

static const char *TAG = "UI";

static lv_obj_t *electrical_page;
static lv_obj_t *environment_page;

static lv_obj_t *voltage_value;
static lv_obj_t *current_value;
static lv_obj_t *power_value;

static lv_obj_t *temp_value;
static lv_obj_t *humidity_value;
static lv_obj_t *smoke_value;

static bool showing_environment_page = false;

static lv_obj_t *create_card(lv_obj_t *parent,
                             const char *name,
                             const char *value,
                             lv_color_t bg_color,
                             lv_obj_t **value_label)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 210, 72);
    lv_obj_set_style_bg_color(card, bg_color, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 10, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, name);
    lv_obj_set_style_text_color(title, lv_color_hex(0x374151), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *val = lv_label_create(card);
    lv_label_set_text(val, value);
    lv_obj_set_style_text_color(val, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_24, 0);
    lv_obj_align(val, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    if (value_label) {
        *value_label = val;
    }

    return card;
}

static lv_obj_t *create_page_container(lv_obj_t *scr)
{
    lv_obj_t *container = lv_obj_create(scr);
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container, 230, 250);
    lv_obj_align(container, LV_ALIGN_CENTER, 0, 22);

    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(container, 12, 0);

    return container;
}

static void page_switch_timer_cb(lv_timer_t *timer)
{
    showing_environment_page = !showing_environment_page;

    if (showing_environment_page) {
        lv_obj_add_flag(electrical_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(environment_page, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(electrical_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(environment_page, LV_OBJ_FLAG_HIDDEN);
    }
}

// ===== UI data update task =====

static void ui_data_task(void *arg)
{
    ESP_LOGI(TAG, "UI data task started");

    vTaskDelay(pdMS_TO_TICKS(500));

    while (1) {
        ina219_data_t ina_data = {0};
        bool ina_ok = ina219_get_latest(&ina_data);

        dht11_data_t dht_data = {0};
        bool dht_ok = dht11_get_latest(&dht_data);

        smoke_data_t smoke_data = {0};
        bool smoke_ok = smoke_sensor_get_latest(&smoke_data);

        lvgl_port_lock();

        char buf[32];

        if (ina_ok) {
            snprintf(buf, sizeof(buf), "%.2f V", ina_data.bus_voltage_v);
            lv_label_set_text(voltage_value, buf);

            snprintf(buf, sizeof(buf), "%.3f A", ina_data.current_a);
            lv_label_set_text(current_value, buf);

            snprintf(buf, sizeof(buf), "%.3f W", ina_data.power_w);
            lv_label_set_text(power_value, buf);
        } else {
            lv_label_set_text(voltage_value, "-- V");
            lv_label_set_text(current_value, "-- A");
            lv_label_set_text(power_value, "-- W");
        }

        if (dht_ok) {
            snprintf(buf, sizeof(buf), "%d C", dht_data.temperature_c);
            lv_label_set_text(temp_value, buf);

            snprintf(buf, sizeof(buf), "%d %%", dht_data.humidity_percent);
            lv_label_set_text(humidity_value, buf);
        } else {
            lv_label_set_text(temp_value, "-- C");
            lv_label_set_text(humidity_value, "-- %");
        }

        if (smoke_ok) {
            snprintf(buf, sizeof(buf), "%d", smoke_data.raw);
            lv_label_set_text(smoke_value, buf);
        } else {
            lv_label_set_text(smoke_value, "--");
        }

        lvgl_port_unlock();

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void ui_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);

    lv_obj_set_style_bg_color(scr, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "SafeCharge");
    lv_obj_set_style_text_color(title, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    // ===== Page 1: electrical parameters =====
    electrical_page = create_page_container(scr);

    create_card(electrical_page,
                "Voltage",
                "-- V",
                lv_color_hex(0xBFDBFE),
                &voltage_value);

    create_card(electrical_page,
                "Current",
                "-- A",
                lv_color_hex(0xBBF7D0),
                &current_value);

    create_card(electrical_page,
                "Power",
                "-- W",
                lv_color_hex(0xFDE68A),
                &power_value);

    // ===== Page 2: environment parameters =====
    environment_page = create_page_container(scr);

    create_card(environment_page,
                "Temperature",
                "-- C",
                lv_color_hex(0xBFDBFE),
                &temp_value);

    create_card(environment_page,
                "Humidity",
                "-- %",
                lv_color_hex(0xBBF7D0),
                &humidity_value);

    create_card(environment_page,
                "Smoke",
                "--",
                lv_color_hex(0xFDE68A),
                &smoke_value);

    lv_obj_add_flag(environment_page, LV_OBJ_FLAG_HIDDEN);

    // Switch pages every 10 seconds.
    lv_timer_create(page_switch_timer_cb, 10000, NULL);
}

void ui_start_env_task(void)
{
    xTaskCreate(ui_data_task, "ui_data_task", 4096, NULL, 4, NULL);
}