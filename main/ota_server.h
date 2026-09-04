#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t ota_server_start(void);
bool ota_server_ready(void);
esp_err_t ota_start_rollback_validation(void);
