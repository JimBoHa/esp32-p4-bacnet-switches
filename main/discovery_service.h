#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "network_config_model.h"

typedef struct {
    bool ready;
    bool https_advertised;
    bool bacnet_advertised;
    esp_err_t last_error;
    uint32_t hostname_conflict_count;
    char hostname[NETWORK_CONFIG_HOSTNAME_LENGTH];
    uint16_t https_port;
    uint16_t bacnet_port;
} discovery_service_snapshot_t;

esp_err_t discovery_service_start(
    const char *hostname,
    const char *instance_name,
    uint16_t https_port,
    uint16_t bacnet_port,
    uint32_t device_instance,
    uint16_t vendor_identifier,
    const char *firmware_version);

bool discovery_service_snapshot_get(discovery_service_snapshot_t *snapshot);
