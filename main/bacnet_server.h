#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_netif.h"

esp_err_t bacnet_server_start(esp_netif_t *netif);
bool bacnet_server_ready(void);
