#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "ina219.h"
#include "dht11.h"
#include "lvgl_port.h"
#include "ui.h"
#include "wifi_manager.h"
#include "http_client_manager.h"
#include "smoke_sensor.h"
#include "relay_manager.h"
#include "safety_manager.h"

static const char *TAG = "SafeCharge";

void app_main(void)
{
    ESP_LOGI(TAG, "SafeCharge ESP32-S3 ST7789 start");

    lvgl_port_init();

    lvgl_port_lock();
    ui_create();
    lvgl_port_refresh_now();
    lvgl_port_unlock();

    lvgl_port_start();
    ina219_start_task();
dht11_start_task();
smoke_sensor_start_task();

relay_manager_start_task();
safety_manager_start_task();

wifi_manager_start_task();
http_client_manager_start_task();

ui_start_env_task();
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
