#include "relay_manager.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ina219.h"
#include "dht11.h"

static const char *TAG = "RELAY";

// ===== Relay GPIO setting =====
// Choose a GPIO that is not used by LCD / I2C / DHT11 / Smoke ADC.
// Current used pins:
// LCD: GPIO7,8,9,10,11,12
// INA219 I2C: GPIO5,6
// DHT11: GPIO4
// Smoke ADC: GPIO1
#define RELAY_GPIO              13

// Many relay modules are active LOW.
// If your relay turns on when GPIO outputs LOW, set this to 0.
// If your relay turns on when GPIO outputs HIGH, set this to 1.
#define RELAY_ACTIVE_LEVEL      1

#define RELAY_CHECK_INTERVAL_MS 500

// Protection thresholds.
// You can tune these later according to your demo load.
#define CURRENT_FAULT_A         2.00f
#define POWER_FAULT_W           10.00f
#define TEMP_FAULT_C            45

static SemaphoreHandle_t s_relay_mutex = NULL;

static bool s_relay_enabled_request = true;
static bool s_relay_on = false;
static relay_fault_t s_fault = RELAY_FAULT_NONE;

static int relay_inactive_level(void)
{
    return RELAY_ACTIVE_LEVEL ? 0 : 1;
}

static void relay_apply_output(bool on)
{
    gpio_set_level(RELAY_GPIO, on ? RELAY_ACTIVE_LEVEL : relay_inactive_level());
    s_relay_on = on;
}

static const char *fault_to_string(relay_fault_t fault)
{
    switch (fault) {
    case RELAY_FAULT_NONE:
        return "None";
    case RELAY_FAULT_OVERCURRENT:
        return "Overcurrent";
    case RELAY_FAULT_OVERPOWER:
        return "Overpower";
    case RELAY_FAULT_OVERTEMP:
        return "Overtemperature";
    default:
        return "Unknown";
    }
}

static relay_fault_t relay_check_fault(void)
{
    ina219_data_t ina_data = {0};
    dht11_data_t dht_data = {0};

    bool ina_ok = ina219_get_latest(&ina_data);
    bool dht_ok = dht11_get_latest(&dht_data);

    if (ina_ok) {
        if (ina_data.current_a > CURRENT_FAULT_A) {
            return RELAY_FAULT_OVERCURRENT;
        }

        if (ina_data.power_w > POWER_FAULT_W) {
            return RELAY_FAULT_OVERPOWER;
        }
    }

    if (dht_ok) {
        if (dht_data.temperature_c > TEMP_FAULT_C) {
            return RELAY_FAULT_OVERTEMP;
        }
    }

    return RELAY_FAULT_NONE;
}

static void relay_gpio_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << RELAY_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));

    // Default safe state: relay off before task logic starts.
    gpio_set_level(RELAY_GPIO, relay_inactive_level());
    s_relay_on = false;

    ESP_LOGI(TAG,
             "Relay initialized on GPIO%d, active_level=%d",
             RELAY_GPIO,
             RELAY_ACTIVE_LEVEL);
}

static void relay_manager_task(void *arg)
{
    ESP_LOGI(TAG, "Relay manager task started");

    relay_gpio_init();

    while (1) {
        relay_fault_t fault = relay_check_fault();

        if (xSemaphoreTake(s_relay_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_fault = fault;

            if (s_fault != RELAY_FAULT_NONE) {
                relay_apply_output(false);

                ESP_LOGW(TAG,
                         "Relay OFF due to fault: %s",
                         fault_to_string(s_fault));
            } else {
                relay_apply_output(s_relay_enabled_request);

                ESP_LOGI(TAG,
                         "Relay %s, fault=%s",
                         s_relay_on ? "ON" : "OFF",
                         fault_to_string(s_fault));
            }

            xSemaphoreGive(s_relay_mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(RELAY_CHECK_INTERVAL_MS));
    }
}

void relay_manager_start_task(void)
{
    if (s_relay_mutex == NULL) {
        s_relay_mutex = xSemaphoreCreateMutex();
    }

    xTaskCreate(relay_manager_task,
                "relay_manager_task",
                4096,
                NULL,
                4,
                NULL);
}

void relay_manager_set_enabled(bool enabled)
{
    if (s_relay_mutex && xSemaphoreTake(s_relay_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_relay_enabled_request = enabled;

        if (!enabled) {
            relay_apply_output(false);
        }

        ESP_LOGI(TAG, "Relay enable request = %s", enabled ? "true" : "false");

        xSemaphoreGive(s_relay_mutex);
    }
}

bool relay_manager_is_on(void)
{
    bool on = false;

    if (s_relay_mutex && xSemaphoreTake(s_relay_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        on = s_relay_on;
        xSemaphoreGive(s_relay_mutex);
    }

    return on;
}

relay_fault_t relay_manager_get_fault(void)
{
    relay_fault_t fault = RELAY_FAULT_NONE;

    if (s_relay_mutex && xSemaphoreTake(s_relay_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        fault = s_fault;
        xSemaphoreGive(s_relay_mutex);
    }

    return fault;
}

const char *relay_manager_get_fault_string(void)
{
    return fault_to_string(relay_manager_get_fault());
}