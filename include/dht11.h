#pragma once

#include <stdbool.h>

#ifndef DHT11_DEFAULT_GPIO
#define DHT11_DEFAULT_GPIO 4
#endif

typedef struct {
    int temperature_c;
    int humidity_percent;
} dht11_data_t;

void dht11_init(int gpio_num);
bool dht11_read(int *temperature_c, int *humidity_percent);

void dht11_start_task(void);
bool dht11_get_latest(dht11_data_t *out_data);