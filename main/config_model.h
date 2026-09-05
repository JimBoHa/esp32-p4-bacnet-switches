#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FIRMWARE_CONFIG_MAGIC 0x50344346U
#define FIRMWARE_CONFIG_SCHEMA 1U
#define FIRMWARE_CONFIG_INPUT_COUNT 3U
#define FIRMWARE_CONFIG_NAME_LENGTH 64U
#define FIRMWARE_CONFIG_VENDOR_NAME_LENGTH 64U
#define FIRMWARE_CONFIG_LOCATION_LENGTH 96U

typedef struct {
    uint32_t magic;
    uint16_t schema;
    uint16_t size;
    uint32_t device_instance;
    uint32_t database_revision;
    uint32_t input_instances[FIRMWARE_CONFIG_INPUT_COUNT];
    uint16_t bacnet_port;
    uint16_t vendor_identifier;
    uint16_t debounce_ms;
    uint8_t input_active_low_mask;
    uint8_t reserved;
    char device_name[FIRMWARE_CONFIG_NAME_LENGTH];
    char vendor_name[FIRMWARE_CONFIG_VENDOR_NAME_LENGTH];
    char location[FIRMWARE_CONFIG_LOCATION_LENGTH];
    char input_names[FIRMWARE_CONFIG_INPUT_COUNT][FIRMWARE_CONFIG_NAME_LENGTH];
    uint32_t crc32;
} firmware_config_t;

bool config_model_validate(
    const firmware_config_t *config,
    char *reason,
    size_t reason_capacity);
uint32_t config_model_crc32(const firmware_config_t *config);
void config_model_finalize(firmware_config_t *config);
bool config_model_is_valid_blob(const firmware_config_t *config);
bool config_model_mutable_equal(
    const firmware_config_t *left,
    const firmware_config_t *right);
