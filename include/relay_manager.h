#pragma once

#include <stdbool.h>

typedef enum {
    RELAY_FAULT_NONE = 0,
    RELAY_FAULT_OVERCURRENT,
    RELAY_FAULT_OVERPOWER,
    RELAY_FAULT_OVERTEMP,
} relay_fault_t;

void relay_manager_start_task(void);

void relay_manager_set_enabled(bool enabled);
bool relay_manager_is_on(void);

relay_fault_t relay_manager_get_fault(void);
const char *relay_manager_get_fault_string(void);