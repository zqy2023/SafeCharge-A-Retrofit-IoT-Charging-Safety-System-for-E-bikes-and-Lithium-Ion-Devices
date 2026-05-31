#pragma once

#include <stdbool.h>

typedef enum {
    SAFETY_STATE_NORMAL = 0,
    SAFETY_STATE_WARNING,
    SAFETY_STATE_FAULT,
    SAFETY_STATE_MANUAL_OFF,
} safety_state_t;

typedef enum {
    SAFETY_FAULT_NONE = 0,
    SAFETY_FAULT_CURRENT,
    SAFETY_FAULT_POWER,
    SAFETY_FAULT_TEMPERATURE,
    SAFETY_FAULT_SMOKE,
} safety_fault_t;

typedef struct {
    safety_state_t state;
    safety_fault_t fault;
    bool smoke_detected;
    bool alarm_active;
    bool fault_latched;
} safety_status_t;

typedef struct {
    float current_warn_a;
    float current_fault_a;
    float power_warn_w;
    float power_fault_w;
    float temp_warn_c;
    float temp_fault_c;
    int smoke_warn_raw;
    int smoke_fault_raw;
} safety_thresholds_t;

void safety_manager_start_task(void);

void safety_manager_set_thresholds(const safety_thresholds_t *thresholds);
void safety_manager_get_thresholds(safety_thresholds_t *out_thresholds);

safety_state_t safety_manager_get_state(void);
safety_fault_t safety_manager_get_fault(void);

const char *safety_manager_get_state_string(void);
const char *safety_manager_get_fault_string(void);

bool safety_manager_is_smoke_detected(void);
bool safety_manager_is_alarm_active(void);
void safety_manager_clear_fault_latch(void);
void safety_manager_get_status(safety_status_t *out_status);
bool safety_manager_is_fault_latched(void);
