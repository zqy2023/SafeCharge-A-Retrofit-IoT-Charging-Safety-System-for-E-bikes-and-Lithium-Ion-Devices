#include "http_client_manager.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi_manager.h"
#include "ina219.h"
#include "dht11.h"
#include "relay_manager.h"
#include "smoke_sensor.h"
#include "safety_manager.h"

static const char *TAG = "HTTP_CLIENT";

// Your Mac SafeCharge web server address
// Use the POST URL printed by app.py, for example:
// ESP32 POST URL: http://192.168.0.122:5050/api/data
#define SERVER_DATA_URL    "http://192.168.2.1:5050/api/data"
#define SERVER_COMMAND_URL "http://192.168.2.1:5050/api/command"

#define HTTP_SYNC_INTERVAL_MS 1000
#define HTTP_START_DELAY_MS   5000
#define HTTP_TIMEOUT_MS       1500
#define HTTP_RESPONSE_BUF_LEN 256

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data && evt->data && evt->data_len > 0) {
        char *response_buffer = (char *)evt->user_data;
        size_t current_len = strlen(response_buffer);
        size_t available = HTTP_RESPONSE_BUF_LEN - 1 - current_len;

        if (available > 0) {
            size_t copy_len = evt->data_len < available ? evt->data_len : available;
            memcpy(response_buffer + current_len, evt->data, copy_len);
            response_buffer[current_len + copy_len] = '\0';
        }
    }

    return ESP_OK;
}

static bool json_get_float(const char *json, const char *key, float *out_value)
{
    if (!json || !key || !out_value) {
        return false;
    }

    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *pos = strstr(json, pattern);
    if (!pos) {
        return false;
    }

    pos = strchr(pos, ':');
    if (!pos) {
        return false;
    }

    pos++;
    *out_value = strtof(pos, NULL);
    return true;
}

static bool json_get_int(const char *json, const char *key, int *out_value)
{
    float value = 0.0f;

    if (!json_get_float(json, key, &value)) {
        return false;
    }

    *out_value = (int)value;
    return true;
}

static void http_apply_threshold_command(const char *response)
{
    safety_thresholds_t th = {0};
    safety_manager_get_thresholds(&th);

    bool changed = false;

    changed |= json_get_float(response, "current_warn", &th.current_warn_a);
    changed |= json_get_float(response, "current_fault", &th.current_fault_a);

    changed |= json_get_float(response, "dc_power_warn", &th.power_warn_w);
    changed |= json_get_float(response, "dc_power_fault", &th.power_fault_w);

    changed |= json_get_float(response, "temp_warn", &th.temp_warn_c);
    changed |= json_get_float(response, "temp_fault", &th.temp_fault_c);

    changed |= json_get_int(response, "smoke_warn", &th.smoke_warn_raw);
    changed |= json_get_int(response, "smoke_fault", &th.smoke_fault_raw);

    // Optional compatibility with alternative backend field names.
    changed |= json_get_float(response, "power_warn", &th.power_warn_w);
    changed |= json_get_float(response, "power_fault", &th.power_fault_w);
    changed |= json_get_int(response, "smoke_warn_raw", &th.smoke_warn_raw);
    changed |= json_get_int(response, "smoke_fault_raw", &th.smoke_fault_raw);

    if (changed) {
        safety_manager_set_thresholds(&th);
    } else {
        ESP_LOGW(TAG, "SET_THRESHOLDS command received, but no supported threshold fields were found");
    }
}

static void http_post_data(void)
{
    if (!wifi_manager_is_connected()) {
        ESP_LOGW(TAG, "WiFi not connected, skip HTTP POST");
        return;
    }

    ina219_data_t ina_data = {0};
    dht11_data_t dht_data = {0};
    smoke_data_t smoke_data = {0};
    safety_status_t safety_status = {0};

    bool ina_ok = ina219_get_latest(&ina_data);
    bool dht_ok = dht11_get_latest(&dht_data);
    bool smoke_ok = smoke_sensor_get_latest(&smoke_data);

    safety_manager_get_status(&safety_status);

    if (!ina_ok) {
        ESP_LOGW(TAG, "INA219 data not ready");
    }

    if (!dht_ok) {
        ESP_LOGW(TAG, "DHT11 data not ready");
    }

    if (!smoke_ok) {
        ESP_LOGW(TAG, "Smoke data not ready");
    }

    int smoke_raw = smoke_ok ? smoke_data.raw : -1;
    bool smoke_detected = safety_status.smoke_detected;
    bool relay_on = relay_manager_is_on();
    const char *state = safety_manager_get_state_string();
    const char *fault = safety_manager_get_fault_string();

    char json_data[512];

    snprintf(json_data,
             sizeof(json_data),
             "{"
             "\"dc_voltage_v\":%.3f,"
             "\"dc_current_a\":%.3f,"
             "\"dc_power_w\":%.3f,"
             "\"current_a\":%.3f,"
             "\"temperature_c\":%d,"
             "\"humidity_pct\":%d,"
             "\"smoke\":%d,"
             "\"smoke_detected\":%s,"
             "\"relay_on\":%s,"
             "\"ina219_ok\":%s,"
             "\"state\":\"%s\","
             "\"fault\":\"%s\","
             "\"alarm_active\":%s,"
             "\"fault_latched\":%s"
             "}",
             ina_ok ? ina_data.bus_voltage_v : 0.0f,
             ina_ok ? ina_data.current_a : 0.0f,
             ina_ok ? ina_data.power_w : 0.0f,
             ina_ok ? ina_data.current_a : 0.0f,
             dht_ok ? dht_data.temperature_c : 0,
             dht_ok ? dht_data.humidity_percent : 0,
             smoke_raw,
             smoke_detected ? "true" : "false",
             relay_on ? "true" : "false",
             ina_ok ? "true" : "false",
             state,
             fault,
             safety_status.alarm_active ? "true" : "false",
             safety_status.fault_latched ? "true" : "false");

    esp_http_client_config_t config = {
        .url = SERVER_DATA_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .timeout_ms = HTTP_TIMEOUT_MS,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_data, strlen(json_data));

    ESP_LOGI(TAG, "POST %s", SERVER_DATA_URL);
    ESP_LOGI(TAG, "JSON: %s", json_data);

    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        int content_length = esp_http_client_get_content_length(client);

        ESP_LOGI(TAG,
                 "HTTP POST finished, status=%d, content_length=%d",
                 status_code,
                 content_length);

        if (status_code >= 200 && status_code < 300) {
            ESP_LOGI(TAG, "HTTP POST success");
        } else {
            ESP_LOGW(TAG, "HTTP POST returned non-2xx status");
        }
    } else {
        ESP_LOGE(TAG,
                 "HTTP POST failed: %s",
                 esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

static void http_handle_command_response(const char *response)
{
    if (response == NULL || strlen(response) == 0) {
        ESP_LOGW(TAG, "Empty command response");
        return;
    }

    ESP_LOGI(TAG, "Command response: %s", response);

    if (strstr(response, "\"command\":null") || strstr(response, "\"command\": null")) {
        ESP_LOGI(TAG, "No pending command");
        return;
    }

    if (strstr(response, "\"command\":\"CUT\"") || strstr(response, "\"command\": \"CUT\"")) {
        ESP_LOGW(TAG, "Command CUT received: relay OFF");
        relay_manager_set_enabled(false);
        return;
    }

    if (strstr(response, "\"command\":\"RESET\"") || strstr(response, "\"command\": \"RESET\"")) {
        ESP_LOGI(TAG, "Command RESET received: clear fault latch and relay ON request");
        safety_manager_clear_fault_latch();
        relay_manager_set_enabled(true);
        return;
    }

    if (strstr(response, "\"command\":\"SET_THRESHOLDS\"") || strstr(response, "\"command\": \"SET_THRESHOLDS\"")) {
        ESP_LOGI(TAG, "Command SET_THRESHOLDS received");
        ESP_LOGI(TAG, "Raw thresholds JSON: %s", response);
        http_apply_threshold_command(response);
        return;
    }

    ESP_LOGW(TAG, "Unknown command response: %s", response);
}

static void http_get_command(void)
{
    if (!wifi_manager_is_connected()) {
        ESP_LOGW(TAG, "WiFi not connected, skip HTTP GET command");
        return;
    }

    char response_buffer[HTTP_RESPONSE_BUF_LEN] = {0};

    esp_http_client_config_t config = {
        .url = SERVER_COMMAND_URL,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .user_data = response_buffer,
        .timeout_ms = HTTP_TIMEOUT_MS,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP command client");
        return;
    }

    ESP_LOGI(TAG, "GET %s", SERVER_COMMAND_URL);

    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);

        ESP_LOGI(TAG, "HTTP GET command finished, status=%d", status_code);

        if (status_code >= 200 && status_code < 300) {
            http_handle_command_response(response_buffer);
        } else {
            ESP_LOGW(TAG, "HTTP GET command returned non-2xx status");
        }
    } else {
        ESP_LOGE(TAG, "HTTP GET command failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

static void http_client_task(void *arg)
{
    ESP_LOGI(TAG, "HTTP client task started");

    vTaskDelay(pdMS_TO_TICKS(HTTP_START_DELAY_MS));

    while (1) {
        http_post_data();
        http_get_command();
        vTaskDelay(pdMS_TO_TICKS(HTTP_SYNC_INTERVAL_MS));
    }
}

void http_client_manager_start_task(void)
{
    xTaskCreate(http_client_task,
                "http_client_task",
                6144,
                NULL,
                3,
                NULL);
}