#include "smoke_sensor.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "SMOKE";

// ===== Smoke sensor analog input =====
// Smoke sensor AO -> GPIO1
// ESP32-S3 GPIO1 = ADC1_CH0
#define SMOKE_ADC_UNIT          ADC_UNIT_1
#define SMOKE_ADC_CHANNEL       ADC_CHANNEL_0
#define SMOKE_READ_INTERVAL_MS  500


static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static SemaphoreHandle_t s_data_mutex = NULL;
static smoke_data_t s_latest_data = {0};
static bool s_has_data = false;

static void smoke_adc_init(void)
{
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = SMOKE_ADC_UNIT,
    };

    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &s_adc_handle));

    adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_11,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle,
                                               SMOKE_ADC_CHANNEL,
                                               &channel_config));

    ESP_LOGI(TAG,
             "Smoke ADC initialized: ADC unit=%d, channel=%d",
             SMOKE_ADC_UNIT,
             SMOKE_ADC_CHANNEL);
}

static bool smoke_read(smoke_data_t *out)
{
    if (!s_adc_handle || !out) {
        return false;
    }

    int raw = 0;
    esp_err_t ret = adc_oneshot_read(s_adc_handle, SMOKE_ADC_CHANNEL, &raw);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Smoke ADC read failed: %s", esp_err_to_name(ret));
        return false;
    }

    float voltage_v = (raw / 4095.0f) * 3.3f;

    out->raw = raw;
    out->voltage_v = voltage_v;
    // This module only reads raw smoke ADC data.
    // Warning/Fault judgement is handled by safety_manager.c.
    out->detected = false;

    return true;
}

static void smoke_task(void *arg)
{
    ESP_LOGI(TAG, "Smoke manager task started");

    smoke_adc_init();

    while (1) {
        smoke_data_t data = {0};

        if (smoke_read(&data)) {
            if (s_data_mutex && xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                s_latest_data = data;
                s_has_data = true;
                xSemaphoreGive(s_data_mutex);
            }

            ESP_LOGI(TAG,
                     "Smoke raw=%d, voltage=%.3f V",
                     data.raw,
                     data.voltage_v);
        }

        vTaskDelay(pdMS_TO_TICKS(SMOKE_READ_INTERVAL_MS));
    }
}

void smoke_sensor_start_task(void)
{
    if (s_data_mutex == NULL) {
        s_data_mutex = xSemaphoreCreateMutex();
    }

    xTaskCreate(smoke_task,
                "smoke_task",
                4096,
                NULL,
                4,
                NULL);
}

bool smoke_sensor_get_latest(smoke_data_t *out_data)
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