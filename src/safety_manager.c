#include "safety_manager.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ina219.h"
#include "dht11.h"
#include "smoke_sensor.h"
#include "relay_manager.h"

static const char *TAG = "SAFETY";

// ===== Alarm output pins =====
// Warning: LED ON
// Fault: LED ON + buzzer ON for 2 seconds, then relay OFF
#define SAFETY_BUZZER_GPIO          14
#define SAFETY_LED_GPIO             15

#define SAFETY_CHECK_INTERVAL_MS    500
#define SAFETY_BUZZER_DURATION_MS   2000

// ===== Local default thresholds =====
// Web SET_THRESHOLDS can update these at runtime through safety_manager_set_thresholds().
#define DEFAULT_CURRENT_WARN_A      0.06f
#define DEFAULT_CURRENT_FAULT_A     0.12f
#define DEFAULT_POWER_WARN_W        7.50f
#define DEFAULT_POWER_FAULT_W       10.00f
#define DEFAULT_TEMP_WARN_C         38.0f
#define DEFAULT_TEMP_FAULT_C        45.0f
#define DEFAULT_SMOKE_WARN_RAW      3400
#define DEFAULT_SMOKE_FAULT_RAW     3600

static SemaphoreHandle_t s_safety_mutex = NULL;

static safety_thresholds_t s_thresholds = {
    .current_warn_a = DEFAULT_CURRENT_WARN_A,
    .current_fault_a = DEFAULT_CURRENT_FAULT_A,
    .power_warn_w = DEFAULT_POWER_WARN_W,
    .power_fault_w = DEFAULT_POWER_FAULT_W,
    .temp_warn_c = DEFAULT_TEMP_WARN_C,
    .temp_fault_c = DEFAULT_TEMP_FAULT_C,
    .smoke_warn_raw = DEFAULT_SMOKE_WARN_RAW,
    .smoke_fault_raw = DEFAULT_SMOKE_FAULT_RAW,
};

static safety_state_t s_state = SAFETY_STATE_NORMAL;
static safety_fault_t s_fault = SAFETY_FAULT_NONE;
static bool s_smoke_detected = false;
static bool s_alarm_active = false;
static bool s_fault_latched = false;

static const char *state_to_string(safety_state_t state)
{
    switch (state) {
    case SAFETY_STATE_NORMAL:
        return "Normal";
    case SAFETY_STATE_WARNING:
        return "Warning";
    case SAFETY_STATE_FAULT:
        return "Fault";
    case SAFETY_STATE_MANUAL_OFF:
        return "ManualOff";
    default:
        return "Unknown";
    }
}

static const char *fault_to_string(safety_fault_t fault)
{
    switch (fault) {
    case SAFETY_FAULT_NONE:
        return "None";
    case SAFETY_FAULT_CURRENT:
        return "Current";
    case SAFETY_FAULT_POWER:
        return "Power";
    case SAFETY_FAULT_TEMPERATURE:
        return "Temperature";
    case SAFETY_FAULT_SMOKE:
        return "Smoke";
    default:
        return "Unknown";
    }
}

static void alarm_gpio_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SAFETY_BUZZER_GPIO) | (1ULL << SAFETY_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));

    gpio_set_level(SAFETY_BUZZER_GPIO, 0);
    gpio_set_level(SAFETY_LED_GPIO, 0);

    ESP_LOGI(TAG,
             "Alarm outputs initialized: buzzer GPIO%d, LED GPIO%d",
             SAFETY_BUZZER_GPIO,
             SAFETY_LED_GPIO);
}

static void led_set(bool on)
{
    gpio_set_level(SAFETY_LED_GPIO, on ? 1 : 0);
}

static void buzzer_set(bool on)
{
    gpio_set_level(SAFETY_BUZZER_GPIO, on ? 1 : 0);

    if (s_safety_mutex && xSemaphoreTake(s_safety_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_alarm_active = on;
        xSemaphoreGive(s_safety_mutex);
    } else {
        s_alarm_active = on;
    }
}

static safety_thresholds_t get_thresholds_copy(void)
{
    safety_thresholds_t th = s_thresholds;

    if (s_safety_mutex && xSemaphoreTake(s_safety_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        th = s_thresholds;
        xSemaphoreGive(s_safety_mutex);
    }

    return th;
}

static void evaluate_sensors(safety_state_t *out_state,
                             safety_fault_t *out_fault,
                             bool *out_smoke_detected)
{
    safety_thresholds_t th = get_thresholds_copy();

    ina219_data_t ina_data = {0};
    dht11_data_t dht_data = {0};
    smoke_data_t smoke_data = {0};

    bool ina_ok = ina219_get_latest(&ina_data);
    bool dht_ok = dht11_get_latest(&dht_data);
    bool smoke_ok = smoke_sensor_get_latest(&smoke_data);

    safety_state_t state = SAFETY_STATE_NORMAL;
    safety_fault_t fault = SAFETY_FAULT_NONE;
    bool smoke_detected = false;

    // Electrical current and power thresholds
    if (ina_ok) {
        if (ina_data.current_a >= th.current_fault_a) {
            state = SAFETY_STATE_FAULT;
            fault = SAFETY_FAULT_CURRENT;
        } else if (ina_data.power_w >= th.power_fault_w) {
            state = SAFETY_STATE_FAULT;
            fault = SAFETY_FAULT_POWER;
        } else if (ina_data.current_a >= th.current_warn_a || ina_data.power_w >= th.power_warn_w) {
            state = SAFETY_STATE_WARNING;
        }
    }

    // Temperature thresholds
    if (state != SAFETY_STATE_FAULT && dht_ok) {
        if ((float)dht_data.temperature_c >= th.temp_fault_c) {
            state = SAFETY_STATE_FAULT;
            fault = SAFETY_FAULT_TEMPERATURE;
        } else if ((float)dht_data.temperature_c >= th.temp_warn_c) {
            state = SAFETY_STATE_WARNING;
        }
    }

    // Smoke thresholds
    if (smoke_ok) {
        smoke_detected = smoke_data.raw >= th.smoke_warn_raw;

        if (smoke_data.raw >= th.smoke_fault_raw) {
            state = SAFETY_STATE_FAULT;
            fault = SAFETY_FAULT_SMOKE;
            smoke_detected = true;
        } else if (state != SAFETY_STATE_FAULT && smoke_data.raw >= th.smoke_warn_raw) {
            state = SAFETY_STATE_WARNING;
        }
    }

    // Manual relay off state from web CUT command or local relay state.
    // If a fault is already latched, keep Fault state instead of ManualOff.
    if (!relay_manager_is_on() && state != SAFETY_STATE_FAULT && !s_fault_latched) {
        state = SAFETY_STATE_MANUAL_OFF;
    }

    if (out_state) {
        *out_state = state;
    }

    if (out_fault) {
        *out_fault = fault;
    }

    if (out_smoke_detected) {
        *out_smoke_detected = smoke_detected;
    }
}

static void safety_manager_task(void *arg)
{
    ESP_LOGI(TAG, "Safety manager task started");

    alarm_gpio_init();

    while (1) {
        safety_state_t evaluated_state = SAFETY_STATE_NORMAL;
        safety_fault_t evaluated_fault = SAFETY_FAULT_NONE;
        bool evaluated_smoke_detected = false;
        bool need_trip = false;

        evaluate_sensors(&evaluated_state, &evaluated_fault, &evaluated_smoke_detected);

        if (xSemaphoreTake(s_safety_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (s_fault_latched) {
                // Once fault occurs, keep Fault state until RESET clears the latch.
                s_state = SAFETY_STATE_FAULT;
            } else {
                s_state = evaluated_state;
                s_fault = evaluated_fault;
            }

            s_smoke_detected = evaluated_smoke_detected;

            if (evaluated_state == SAFETY_STATE_FAULT && !s_fault_latched) {
                s_fault_latched = true;
                s_state = SAFETY_STATE_FAULT;
                s_fault = evaluated_fault;
                need_trip = true;
            }

            xSemaphoreGive(s_safety_mutex);
        }

        safety_state_t current_state = safety_manager_get_state();

        // Warning: LED ON, no buzzer, no relay cut.
        // Fault: LED ON, buzzer for 2 seconds, then relay cut.
        led_set(current_state != SAFETY_STATE_NORMAL);

        if (need_trip) {
            ESP_LOGE(TAG,
                     "Fault=%s. Buzzer for %d ms, then cut relay.",
                     fault_to_string(evaluated_fault),
                     SAFETY_BUZZER_DURATION_MS);

            buzzer_set(true);
            vTaskDelay(pdMS_TO_TICKS(SAFETY_BUZZER_DURATION_MS));
            buzzer_set(false);

            relay_manager_set_enabled(false);
        }

        ESP_LOGI(TAG,
                 "state=%s, fault=%s, smoke=%s, alarm=%s, latched=%s",
                 safety_manager_get_state_string(),
                 safety_manager_get_fault_string(),
                 safety_manager_is_smoke_detected() ? "true" : "false",
                 safety_manager_is_alarm_active() ? "true" : "false",
                 safety_manager_is_fault_latched() ? "true" : "false");

        vTaskDelay(pdMS_TO_TICKS(SAFETY_CHECK_INTERVAL_MS));
    }
}

void safety_manager_start_task(void)
{
    if (s_safety_mutex == NULL) {
        s_safety_mutex = xSemaphoreCreateMutex();
    }

    xTaskCreate(safety_manager_task,
                "safety_task",
                4096,
                NULL,
                5,
                NULL);
}

void safety_manager_set_thresholds(const safety_thresholds_t *thresholds)
{
    if (!thresholds) {
        return;
    }

    if (s_safety_mutex && xSemaphoreTake(s_safety_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_thresholds = *thresholds;
        xSemaphoreGive(s_safety_mutex);
    }

    ESP_LOGI(TAG,
             "Thresholds updated: current %.3f/%.3f A, power %.3f/%.3f W, temp %.1f/%.1f C, smoke %d/%d",
             thresholds->current_warn_a,
             thresholds->current_fault_a,
             thresholds->power_warn_w,
             thresholds->power_fault_w,
             thresholds->temp_warn_c,
             thresholds->temp_fault_c,
             thresholds->smoke_warn_raw,
             thresholds->smoke_fault_raw);
}

void safety_manager_get_thresholds(safety_thresholds_t *out_thresholds)
{
    if (!out_thresholds) {
        return;
    }

    *out_thresholds = get_thresholds_copy();
}

void safety_manager_clear_fault_latch(void)
{
    if (s_safety_mutex && xSemaphoreTake(s_safety_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_fault_latched = false;
        s_fault = SAFETY_FAULT_NONE;
        s_state = SAFETY_STATE_NORMAL;
        s_smoke_detected = false;
        xSemaphoreGive(s_safety_mutex);
    }

    led_set(false);
    buzzer_set(false);

    ESP_LOGI(TAG, "Fault latch cleared");
}

safety_state_t safety_manager_get_state(void)
{
    safety_state_t state = SAFETY_STATE_NORMAL;

    if (s_safety_mutex && xSemaphoreTake(s_safety_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        state = s_state;
        xSemaphoreGive(s_safety_mutex);
    }

    return state;
}

safety_fault_t safety_manager_get_fault(void)
{
    safety_fault_t fault = SAFETY_FAULT_NONE;

    if (s_safety_mutex && xSemaphoreTake(s_safety_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        fault = s_fault;
        xSemaphoreGive(s_safety_mutex);
    }

    return fault;
}

void safety_manager_get_status(safety_status_t *out_status)
{
    if (!out_status) {
        return;
    }

    if (s_safety_mutex && xSemaphoreTake(s_safety_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        out_status->state = s_state;
        out_status->fault = s_fault;
        out_status->smoke_detected = s_smoke_detected;
        out_status->alarm_active = s_alarm_active;
        out_status->fault_latched = s_fault_latched;
        xSemaphoreGive(s_safety_mutex);
    }
}

const char *safety_manager_get_state_string(void)
{
    return state_to_string(safety_manager_get_state());
}

const char *safety_manager_get_fault_string(void)
{
    return fault_to_string(safety_manager_get_fault());
}

bool safety_manager_is_smoke_detected(void)
{
    bool detected = false;

    if (s_safety_mutex && xSemaphoreTake(s_safety_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        detected = s_smoke_detected;
        xSemaphoreGive(s_safety_mutex);
    }

    return detected;
}

bool safety_manager_is_alarm_active(void)
{
    bool active = false;

    if (s_safety_mutex && xSemaphoreTake(s_safety_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        active = s_alarm_active;
        xSemaphoreGive(s_safety_mutex);
    }

    return active;
}

bool safety_manager_is_fault_latched(void)
{
    bool latched = false;

    if (s_safety_mutex && xSemaphoreTake(s_safety_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        latched = s_fault_latched;
        xSemaphoreGive(s_safety_mutex);
    }

    return latched;
}