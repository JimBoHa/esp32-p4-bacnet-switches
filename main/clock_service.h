#pragma once

#include "clock_model.h"
#include "esp_err.h"

typedef struct {
    clock_model_t clock;
    clock_stamp_t now;
    bool initialized;
    int32_t last_error;
} clock_service_snapshot_t;

esp_err_t clock_service_init(void);
esp_err_t clock_service_start(void);
clock_stamp_t clock_service_stamp(uint64_t uptime_ms);
void clock_service_snapshot_get(clock_service_snapshot_t *snapshot);
const char *clock_service_configured_server(void);
