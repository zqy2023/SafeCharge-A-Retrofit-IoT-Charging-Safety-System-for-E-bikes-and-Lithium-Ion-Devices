#pragma once

#include <stdbool.h>

typedef struct {
    int raw;
    float voltage_v;
    bool detected;
} smoke_data_t;

void smoke_sensor_start_task(void);
bool smoke_sensor_get_latest(smoke_data_t *out_data);