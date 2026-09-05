#pragma once

#include <stdbool.h>

#include "config_model.h"
#include "esp_err.h"

esp_err_t config_store_init(void);
void config_store_get_active(firmware_config_t *config);
void config_store_get_saved(firmware_config_t *config);
esp_err_t config_store_update(
    const firmware_config_t *config,
    bool *changed);
bool config_store_restart_required(void);
