#include "dht11.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"

static const char *TAG = "DHT11";
static int s_dht11_gpio = -1;
static portMUX_TYPE s_dht11_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_data_mutex = NULL;
static dht11_data_t s_latest_data = {0};
static bool s_has_data = false;

void dht11_init(int gpio_num)
{
    s_dht11_gpio = gpio_num;

    gpio_set_pull_mode(s_dht11_gpio, GPIO_PULLUP_ONLY);
    gpio_set_direction(s_dht11_gpio, GPIO_MODE_INPUT);

    ESP_LOGI(TAG,
             "DHT11 init on GPIO%d, idle level=%d",
             s_dht11_gpio,
             gpio_get_level(s_dht11_gpio));
}

static bool wait_level(int level, uint32_t timeout_us)
{
    int64_t start = esp_timer_get_time();

    while (gpio_get_level(s_dht11_gpio) != level) {
        if ((uint32_t)(esp_timer_get_time() - start) > timeout_us) {
            return false;
        }
    }

    return true;
}

static int measure_high_us(uint32_t timeout_us)
{
    int64_t start = esp_timer_get_time();

    while (gpio_get_level(s_dht11_gpio) == 1) {
        if ((uint32_t)(esp_timer_get_time() - start) > timeout_us) {
            return -1;
        }
    }

    return (int)(esp_timer_get_time() - start);
}

bool dht11_read(int *temperature_c, int *humidity_percent)
{
    if (s_dht11_gpio < 0) {
        ESP_LOGE(TAG, "DHT11 not initialized");
        return false;
    }

    uint8_t data[5] = {0};

    gpio_set_pull_mode(s_dht11_gpio, GPIO_PULLUP_ONLY);
    gpio_set_direction(s_dht11_gpio, GPIO_MODE_INPUT);
    vTaskDelay(pdMS_TO_TICKS(10));

    gpio_set_direction(s_dht11_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(s_dht11_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    gpio_set_level(s_dht11_gpio, 1);
    esp_rom_delay_us(40);
    gpio_set_direction(s_dht11_gpio, GPIO_MODE_INPUT);
    gpio_set_pull_mode(s_dht11_gpio, GPIO_PULLUP_ONLY);

    // DHT11 timing is in microseconds. Keep this section short and protected
    // so LVGL/Wi-Fi tasks do not interrupt the 40-bit read and cause checksum errors.
    portENTER_CRITICAL(&s_dht11_mux);

    if (!wait_level(0, 200)) {
        portEXIT_CRITICAL(&s_dht11_mux);
        ESP_LOGW(TAG, "No response LOW, idle=%d", gpio_get_level(s_dht11_gpio));
        return false;
    }

    if (!wait_level(1, 200)) {
        portEXIT_CRITICAL(&s_dht11_mux);
        ESP_LOGW(TAG, "No response HIGH");
        return false;
    }

    if (!wait_level(0, 200)) {
        portEXIT_CRITICAL(&s_dht11_mux);
        ESP_LOGW(TAG, "No data start LOW");
        return false;
    }

    for (int i = 0; i < 40; i++) {
        if (!wait_level(1, 120)) {
            portEXIT_CRITICAL(&s_dht11_mux);
            ESP_LOGW(TAG, "Bit %d no HIGH", i);
            return false;
        }

        int high_time = measure_high_us(120);
        if (high_time < 0) {
            portEXIT_CRITICAL(&s_dht11_mux);
            ESP_LOGW(TAG, "Bit %d HIGH timeout", i);
            return false;
        }

        data[i / 8] <<= 1;

        if (high_time > 45) {
            data[i / 8] |= 1;
        }
    }

    portEXIT_CRITICAL(&s_dht11_mux);

    uint8_t checksum = data[0] + data[1] + data[2] + data[3];

    if (checksum != data[4]) {
        ESP_LOGW(TAG,
                 "Checksum failed: %02X %02X %02X %02X %02X, sum=%02X",
                 data[0], data[1], data[2], data[3], data[4], checksum);
        return false;
    }

    *humidity_percent = data[0];
    *temperature_c = data[2];

    ESP_LOGI(TAG,
             "Temperature=%d C, Humidity=%d %%",
             *temperature_c,
             *humidity_percent);

    return true;
}

static void dht11_task(void *arg)
{
    dht11_init(DHT11_DEFAULT_GPIO);

    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1) {
        int temperature = 0;
        int humidity = 0;

        if (dht11_read(&temperature, &humidity)) {
            if (s_data_mutex && xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                s_latest_data.temperature_c = temperature;
                s_latest_data.humidity_percent = humidity;
                s_has_data = true;
                xSemaphoreGive(s_data_mutex);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void dht11_start_task(void)
{
    if (s_data_mutex == NULL) {
        s_data_mutex = xSemaphoreCreateMutex();
    }

    xTaskCreate(dht11_task, "dht11_task", 4096, NULL, 6, NULL);
}

bool dht11_get_latest(dht11_data_t *out_data)
{
    if (!out_data || !s_data_mutex) {
        return false;
    }

    bool ok = false;

    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (s_has_data) {
            *out_data = s_latest_data;
            ok = true;
        }

        xSemaphoreGive(s_data_mutex);
    }

    return ok;
}
