#include "config_model.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#ifdef ESP_PLATFORM
#include "esp_crc.h"
#else
static uint32_t host_crc32_le(
    uint32_t crc,
    const uint8_t *data,
    size_t length)
{
    crc = ~crc;
    for (size_t index = 0; index < length; ++index) {
        crc ^= data[index];
        for (unsigned bit = 0; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^
                (0xEDB88320U & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}
#define esp_crc32_le host_crc32_le
#endif

_Static_assert(
    offsetof(firmware_config_t, crc32) + sizeof(uint32_t) ==
        sizeof(firmware_config_t),
    "configuration CRC must be the final field");

static void set_reason(char *reason, size_t capacity, const char *message)
{
    if (reason != NULL && capacity > 0U) {
        (void)snprintf(reason, capacity, "%s", message);
    }
}

static bool printable_text(
    const char *text,
    size_t capacity,
    bool allow_empty)
{
    if (text == NULL || memchr(text, '\0', capacity) == NULL ||
        (!allow_empty && text[0] == '\0')) {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor != '\0';
         ++cursor) {
        if (*cursor < 0x20U || *cursor > 0x7EU) {
            return false;
        }
    }
    return true;
}

static bool object_names_unique(const firmware_config_t *config)
{
    const char *names[1U + FIRMWARE_CONFIG_INPUT_COUNT] = {
        config->device_name,
        config->input_names[0],
        config->input_names[1],
        config->input_names[2],
    };
    for (size_t left = 0U;
         left < sizeof(names) / sizeof(names[0]);
         ++left) {
        for (size_t right = left + 1U;
             right < sizeof(names) / sizeof(names[0]);
             ++right) {
            if (strcasecmp(names[left], names[right]) == 0) {
                return false;
            }
        }
    }
    return true;
}

bool config_model_validate(
    const firmware_config_t *config,
    char *reason,
    size_t reason_capacity)
{
    if (config == NULL) {
        set_reason(reason, reason_capacity, "missing configuration");
        return false;
    }
    if (config->device_instance >= 4194303U) {
        set_reason(reason, reason_capacity, "device_instance must be 0..4194302");
        return false;
    }
    if (config->bacnet_port == 0U) {
        set_reason(reason, reason_capacity, "bacnet_port must be 1..65535");
        return false;
    }
    if (config->debounce_ms < 10U || config->debounce_ms > 500U) {
        set_reason(reason, reason_capacity, "debounce_ms must be 10..500");
        return false;
    }
    if ((config->input_active_low_mask &
            ~((1U << FIRMWARE_CONFIG_INPUT_COUNT) - 1U)) != 0U) {
        set_reason(reason, reason_capacity, "input_active_low_mask has unknown bits");
        return false;
    }
    if (!printable_text(
            config->device_name, sizeof(config->device_name), false)) {
        set_reason(reason, reason_capacity, "device_name must be printable ASCII");
        return false;
    }
    if (!printable_text(
            config->vendor_name, sizeof(config->vendor_name), false)) {
        set_reason(reason, reason_capacity, "vendor_name must be printable ASCII");
        return false;
    }
    if (!printable_text(
            config->location, sizeof(config->location), true)) {
        set_reason(reason, reason_capacity, "location must be printable ASCII");
        return false;
    }
    for (size_t index = 0U; index < FIRMWARE_CONFIG_INPUT_COUNT; ++index) {
        if (config->input_instances[index] >= 4194303U) {
            set_reason(reason, reason_capacity, "input instance must be 0..4194302");
            return false;
        }
        if (!printable_text(
                config->input_names[index],
                sizeof(config->input_names[index]),
                false)) {
            set_reason(reason, reason_capacity, "input names must be printable ASCII");
            return false;
        }
        for (size_t prior = 0U; prior < index; ++prior) {
            if (config->input_instances[index] ==
                config->input_instances[prior]) {
                set_reason(reason, reason_capacity, "input instances must be unique");
                return false;
            }
        }
    }
    if (!object_names_unique(config)) {
        set_reason(reason, reason_capacity, "BACnet object names must be unique");
        return false;
    }
    set_reason(reason, reason_capacity, "ok");
    return true;
}

uint32_t config_model_crc32(const firmware_config_t *config)
{
    return config != NULL
        ? esp_crc32_le(
              0U,
              (const uint8_t *)config,
              offsetof(firmware_config_t, crc32))
        : 0U;
}

void config_model_finalize(firmware_config_t *config)
{
    if (config == NULL) {
        return;
    }
    config->magic = FIRMWARE_CONFIG_MAGIC;
    config->schema = FIRMWARE_CONFIG_SCHEMA;
    config->size = (uint16_t)sizeof(*config);
    config->reserved = 0U;
    config->crc32 = config_model_crc32(config);
}

bool config_model_is_valid_blob(const firmware_config_t *config)
{
    return config != NULL && config->magic == FIRMWARE_CONFIG_MAGIC &&
        config->schema == FIRMWARE_CONFIG_SCHEMA &&
        config->size == sizeof(*config) &&
        config->crc32 == config_model_crc32(config) &&
        config_model_validate(config, NULL, 0U);
}

bool config_model_mutable_equal(
    const firmware_config_t *left,
    const firmware_config_t *right)
{
    if (left == NULL || right == NULL) {
        return false;
    }
    if (left->device_instance != right->device_instance ||
        left->bacnet_port != right->bacnet_port ||
        left->vendor_identifier != right->vendor_identifier ||
        left->debounce_ms != right->debounce_ms ||
        left->input_active_low_mask != right->input_active_low_mask ||
        strcmp(left->device_name, right->device_name) != 0 ||
        strcmp(left->vendor_name, right->vendor_name) != 0 ||
        strcmp(left->location, right->location) != 0) {
        return false;
    }
    for (size_t index = 0U; index < FIRMWARE_CONFIG_INPUT_COUNT; ++index) {
        if (left->input_instances[index] != right->input_instances[index] ||
            strcmp(left->input_names[index], right->input_names[index]) != 0) {
            return false;
        }
    }
    return true;
}
