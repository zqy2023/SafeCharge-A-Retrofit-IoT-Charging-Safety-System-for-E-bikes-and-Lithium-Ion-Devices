#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "WiFi";

// ===== Wi-Fi settings =====
// Change these two strings before uploading.
#define WIFI_SSID      "SafeCharge"
#define WIFI_PASSWORD  "SafeCharge"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;
static SemaphoreHandle_t s_wifi_mutex = NULL;

static bool s_wifi_connected = false;
static char s_ip_string[16] = "0.0.0.0";
static int s_retry_count = 0;

static void wifi_set_status(bool connected, const char *ip_string)
{
    if (s_wifi_mutex && xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_wifi_connected = connected;

        if (ip_string) {
            strncpy(s_ip_string, ip_string, sizeof(s_ip_string) - 1);
            s_ip_string[sizeof(s_ip_string) - 1] = '\0';
        } else if (!connected) {
            strncpy(s_ip_string, "0.0.0.0", sizeof(s_ip_string) - 1);
            s_ip_string[sizeof(s_ip_string) - 1] = '\0';
        }

        xSemaphoreGive(s_wifi_mutex);
    }
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA started");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "WiFi disconnected, reason=%d", event->reason);

        wifi_set_status(false, NULL);

        if (s_retry_count < 10) {
            s_retry_count++;
            ESP_LOGI(TAG, "Retrying WiFi connection... attempt %d", s_retry_count);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "WiFi connection failed after retries");

            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        char ip_buf[16];
        snprintf(ip_buf, sizeof(ip_buf), IPSTR, IP2STR(&event->ip_info.ip));

        ESP_LOGI(TAG, "WiFi connected, IP=%s", ip_buf);

        s_retry_count = 0;
        wifi_set_status(true, ip_buf);

        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

static esp_err_t wifi_storage_init(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    return ret;
}

static esp_err_t wifi_init_sta(void)
{
    ESP_ERROR_CHECK(wifi_storage_init());

    ESP_ERROR_CHECK(esp_netif_init());

    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create default WiFi STA netif");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        wifi_event_handler,
                                                        NULL,
                                                        NULL));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(15000)
    );

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connection ready");
        return ESP_OK;
    }

    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "WiFi connection failed");
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "WiFi connection timeout, continuing in background");
    return ESP_ERR_TIMEOUT;
}

static void wifi_manager_task(void *arg)
{
    ESP_LOGI(TAG, "WiFi manager task started");

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create WiFi event group");
        vTaskDelete(NULL);
        return;
    }

    if (s_wifi_mutex == NULL) {
        s_wifi_mutex = xSemaphoreCreateMutex();
    }

    esp_err_t ret = wifi_init_sta();

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SafeCharge network online");
    } else {
        ESP_LOGW(TAG, "SafeCharge network not ready yet, err=%s", esp_err_to_name(ret));
    }

    while (1) {
        if (wifi_manager_is_connected()) {
            ESP_LOGI(TAG, "WiFi status: CONNECTED, IP=%s", wifi_manager_get_ip_string());
        } else {
            ESP_LOGW(TAG, "WiFi status: NOT CONNECTED");
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void wifi_manager_start_task(void)
{
    xTaskCreate(
        wifi_manager_task,
        "wifi_manager_task",
        4096,
        NULL,
        4,
        NULL
    );
}

bool wifi_manager_is_connected(void)
{
    bool connected = false;

    if (s_wifi_mutex && xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        connected = s_wifi_connected;
        xSemaphoreGive(s_wifi_mutex);
    }

    return connected;
}

const char *wifi_manager_get_ip_string(void)
{
    return s_ip_string;
}