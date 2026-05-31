#pragma once

#include <stdbool.h>

typedef struct {
    float bus_voltage_v;
    float shunt_voltage_mv;
    float current_a;
    float power_w;
} ina219_data_t;

void ina219_start_task(void);
bool ina219_get_latest(ina219_data_t *out_data);