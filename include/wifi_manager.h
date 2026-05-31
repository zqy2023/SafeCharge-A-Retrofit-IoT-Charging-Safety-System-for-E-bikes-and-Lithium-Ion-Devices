#pragma once

#include <stdbool.h>

void wifi_manager_start_task(void);
bool wifi_manager_is_connected(void);
const char *wifi_manager_get_ip_string(void);