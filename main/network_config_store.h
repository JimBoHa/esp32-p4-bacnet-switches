#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "network_config_model.h"

esp_err_t network_config_store_init(void);
void network_config_get_active(network_config_t *config);
void network_config_get_saved(network_config_t *config);
void network_config_get_confirmed(network_config_t *config);
esp_err_t network_config_update(
    const network_config_t *config,
    bool *changed);
bool network_config_restart_required(void);
bool network_config_trial_active(void);
uint32_t network_config_trial_seconds_remaining(void);
esp_err_t network_config_confirm_trial(void);
esp_err_t network_config_start_trial_guard(void);
