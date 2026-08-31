#pragma once

#include "esp_err.h"

esp_err_t ota_server_start(void);
esp_err_t ota_start_rollback_validation(void);
