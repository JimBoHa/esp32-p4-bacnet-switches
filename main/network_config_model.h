#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NETWORK_CONFIG_MAGIC 0x50344E43U
#define NETWORK_CONFIG_SCHEMA 1U
#define NETWORK_CONFIG_HOSTNAME_LENGTH 64U
#define NETWORK_CONFIG_TRIAL_TIMEOUT_SECONDS 60U

typedef enum {
    NETWORK_ADDRESS_DHCP = 0,
    NETWORK_ADDRESS_STATIC = 1,
} network_address_mode_t;

typedef struct {
    uint32_t magic;
    uint16_t schema;
    uint16_t size;
    uint32_t revision;
    uint8_t mode;
    uint8_t reserved[3];
    char hostname[NETWORK_CONFIG_HOSTNAME_LENGTH];
    uint8_t ipv4[4];
    uint8_t netmask[4];
    uint8_t gateway[4];
    uint8_t dns[4];
    uint32_t crc32;
} network_config_t;

bool network_config_validate(
    const network_config_t *config,
    char *reason,
    size_t reason_capacity);
uint32_t network_config_crc32(const network_config_t *config);
void network_config_finalize(network_config_t *config);
bool network_config_is_valid_blob(const network_config_t *config);
bool network_config_mutable_equal(
    const network_config_t *left,
    const network_config_t *right);
