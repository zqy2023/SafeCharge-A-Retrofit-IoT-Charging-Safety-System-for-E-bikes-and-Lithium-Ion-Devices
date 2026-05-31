#include "ina219.h"

#include <stdint.h>

#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "INA219";

// ===== INA219 I2C pins =====
// INA219 VCC -> ESP32-S3 3.3V
// INA219 GND -> ESP32-S3 GND
// INA219 SDA -> GPIO5
// INA219 SCL -> GPIO6
#define INA219_I2C_PORT       I2C_NUM_0
#define INA219_SDA_GPIO       5
#define INA219_SCL_GPIO       6
#define INA219_I2C_FREQ_HZ    100000

#define INA219_ADDR           0x40

#define INA219_REG_CONFIG     0x00
#define INA219_REG_SHUNT_V    0x01
#define INA219_REG_BUS_V      0x02

// R100 = 0.1 ohm
#define INA219_SHUNT_OHMS     0.120f

static SemaphoreHandle_t s_ina219_mutex = NULL;
static ina219_data_t s_latest_data = {0};

static bool s_has_data = false;

static float clamp_non_negative(float value)
{
    if (value < 0.0f) {
        return 0.0f;
    }

    return value;
}

static esp_err_t ina219_write_reg(uint8_t reg, uint16_t value)
{
    uint8_t data[3] = {
        reg,
        (uint8_t)(value >> 8),
        (uint8_t)(value & 0xFF),
    };

    return i2c_master_write_to_device(
        INA219_I2C_PORT,
        INA219_ADDR,
        data,
        sizeof(data),
        pdMS_TO_TICKS(100)
    );
}

static esp_err_t ina219_read_reg(uint8_t reg, uint16_t *value)
{
    uint8_t data[2] = {0};

    esp_err_t ret = i2c_master_write_read_device(
        INA219_I2C_PORT,
        INA219_ADDR,
        &reg,
        1,
        data,
        2,
        pdMS_TO_TICKS(100)
    );

    if (ret != ESP_OK) {
        return ret;
    }

    *value = ((uint16_t)data[0] << 8) | data[1];
    return ESP_OK;
}

static esp_err_t ina219_i2c_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = INA219_SDA_GPIO,
        .scl_io_num = INA219_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = INA219_I2C_FREQ_HZ,
    };

    ESP_ERROR_CHECK(i2c_param_config(INA219_I2C_PORT, &conf));

    esp_err_t ret = i2c_driver_install(
        INA219_I2C_PORT,
        I2C_MODE_MASTER,
        0,
        0,
        0
    );

    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "I2C driver already installed");
        return ESP_OK;
    }

    return ret;
}

static esp_err_t ina219_init(void)
{
    ESP_ERROR_CHECK(ina219_i2c_init());

    /*
     * Config 0x399F:
     * Bus voltage range: 32V
     * PGA: +/-320mV
     * Bus ADC: 12-bit
     * Shunt ADC: 12-bit
     * Mode: shunt and bus continuous
     */
    uint16_t config = 0x399F;

    esp_err_t ret = ina219_write_reg(INA219_REG_CONFIG, config);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "INA219 not found at address 0x%02X, err=%s",
                 INA219_ADDR,
                 esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG,
             "INA219 initialized. SDA=GPIO%d, SCL=GPIO%d, address=0x%02X",
             INA219_SDA_GPIO,
             INA219_SCL_GPIO,
             INA219_ADDR);

    return ESP_OK;
}

static bool ina219_read_values(ina219_data_t *data)
{
    uint16_t raw_bus = 0;
    uint16_t raw_shunt = 0;

    esp_err_t ret = ina219_read_reg(INA219_REG_BUS_V, &raw_bus);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Read bus voltage failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = ina219_read_reg(INA219_REG_SHUNT_V, &raw_shunt);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Read shunt voltage failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Bus voltage register: bit[15:3], LSB = 4mV
    float bus_voltage_v = ((raw_bus >> 3) * 4.0f) / 1000.0f;

    // Shunt voltage register: signed, LSB = 10uV
    int16_t shunt_signed = (int16_t)raw_shunt;
    float shunt_voltage_v = shunt_signed * 10.0f / 1000000.0f;
    float shunt_voltage_mv = shunt_voltage_v * 1000.0f;

    float current_a = shunt_voltage_v / INA219_SHUNT_OHMS;
    float power_w = bus_voltage_v * current_a;

    // For this project, reverse-current/noise readings are displayed and uploaded as 0.
    // This keeps all electrical parameters non-negative.
    data->bus_voltage_v = clamp_non_negative(bus_voltage_v);
    data->shunt_voltage_mv = clamp_non_negative(shunt_voltage_mv);
    data->current_a = clamp_non_negative(current_a);
    data->power_w = clamp_non_negative(power_w);

    return true;
}

static void ina219_task(void *arg)
{
    if (ina219_init() != ESP_OK) {
        ESP_LOGE(TAG, "INA219 init failed, task stopped");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        ina219_data_t data = {0};

        if (ina219_read_values(&data)) {
            if (s_ina219_mutex &&
                xSemaphoreTake(s_ina219_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {

                s_latest_data = data;
                s_has_data = true;

                xSemaphoreGive(s_ina219_mutex);
            }

            ESP_LOGI(TAG,
                     "Bus=%.3f V | Shunt=%.3f mV | Current=%.3f A | Power=%.3f W",
                     data.bus_voltage_v,
                     data.shunt_voltage_mv,
                     data.current_a,
                     data.power_w);
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void ina219_start_task(void)
{
    if (s_ina219_mutex == NULL) {
        s_ina219_mutex = xSemaphoreCreateMutex();
    }

    xTaskCreate(
        ina219_task,
        "ina219_task",
        4096,
        NULL,
        4,
        NULL
    );
}

bool ina219_get_latest(ina219_data_t *out_data)
{
    if (!out_data || !s_ina219_mutex) {
        return false;
    }

    bool ok = false;

    if (xSemaphoreTake(s_ina219_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (s_has_data) {
            *out_data = s_latest_data;
            ok = true;
        }

        xSemaphoreGive(s_ina219_mutex);
    }

    return ok;
}