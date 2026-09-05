#include "config_store.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"

#ifndef CONFIG_TOGGLE_INPUT_1_ACTIVE_LOW
#define CONFIG_TOGGLE_INPUT_1_ACTIVE_LOW 0
#endif
#ifndef CONFIG_TOGGLE_INPUT_2_ACTIVE_LOW
#define CONFIG_TOGGLE_INPUT_2_ACTIVE_LOW 0
#endif
#ifndef CONFIG_TOGGLE_INPUT_3_ACTIVE_LOW
#define CONFIG_TOGGLE_INPUT_3_ACTIVE_LOW 0
#endif

#define CONFIG_NVS_NAMESPACE "p4_config"
#define CONFIG_NVS_KEY "current"

static const char *TAG = "config_store";
static SemaphoreHandle_t config_mutex;
static firmware_config_t active_config;
static firmware_config_t saved_config;
static atomic_bool restart_required;

static void factory_defaults(firmware_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->device_instance = CONFIG_BACNET_DEVICE_INSTANCE;
    config->database_revision = 3U;
    config->input_instances[0] = CONFIG_TOGGLE_INPUT_1_OBJECT_INSTANCE;
    config->input_instances[1] = CONFIG_TOGGLE_INPUT_2_OBJECT_INSTANCE;
    config->input_instances[2] = CONFIG_TOGGLE_INPUT_3_OBJECT_INSTANCE;
    config->bacnet_port = CONFIG_BACNET_UDP_PORT;
    config->vendor_identifier = CONFIG_BACNET_VENDOR_IDENTIFIER;
    config->debounce_ms = CONFIG_TOGGLE_DEBOUNCE_MS;
    config->input_active_low_mask =
        (CONFIG_TOGGLE_INPUT_1_ACTIVE_LOW ? 1U : 0U) |
        (CONFIG_TOGGLE_INPUT_2_ACTIVE_LOW ? 2U : 0U) |
        (CONFIG_TOGGLE_INPUT_3_ACTIVE_LOW ? 4U : 0U);
    (void)snprintf(
        config->device_name,
        sizeof(config->device_name),
        "%s",
        CONFIG_BACNET_DEVICE_NAME);
    (void)snprintf(
        config->vendor_name,
        sizeof(config->vendor_name),
        "%s",
        CONFIG_BACNET_VENDOR_NAME);
    (void)snprintf(config->location, sizeof(config->location), "Uncommissioned");
    (void)snprintf(
        config->input_names[0],
        sizeof(config->input_names[0]),
        "%s",
        CONFIG_TOGGLE_INPUT_1_NAME);
    (void)snprintf(
        config->input_names[1],
        sizeof(config->input_names[1]),
        "%s",
        CONFIG_TOGGLE_INPUT_2_NAME);
    (void)snprintf(
        config->input_names[2],
        sizeof(config->input_names[2]),
        "%s",
        CONFIG_TOGGLE_INPUT_3_NAME);
    config_model_finalize(config);
}

static esp_err_t write_saved_config(const firmware_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(
        CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    result = nvs_set_blob(handle, CONFIG_NVS_KEY, config, sizeof(*config));
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

esp_err_t config_store_init(void)
{
    config_mutex = xSemaphoreCreateMutex();
    if (config_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    bool loaded = false;
    nvs_handle_t handle;
    esp_err_t result = nvs_open(
        CONFIG_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (result == ESP_OK) {
        size_t stored_size = sizeof(saved_config);
        result = nvs_get_blob(
            handle, CONFIG_NVS_KEY, &saved_config, &stored_size);
        nvs_close(handle);
        loaded = result == ESP_OK && stored_size == sizeof(saved_config) &&
            config_model_is_valid_blob(&saved_config);
    }

    if (!loaded) {
        factory_defaults(&saved_config);
        result = write_saved_config(&saved_config);
        if (result != ESP_OK) {
            return result;
        }
        ESP_LOGW(TAG, "installed validated factory configuration");
    }
    active_config = saved_config;
    atomic_init(&restart_required, false);
    ESP_LOGI(
        TAG,
        "loaded configuration revision %u for BACnet Device %u",
        (unsigned)active_config.database_revision,
        (unsigned)active_config.device_instance);
    return ESP_OK;
}

void config_store_get_active(firmware_config_t *config)
{
    if (config == NULL || config_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(config_mutex, portMAX_DELAY) == pdTRUE) {
        *config = active_config;
        xSemaphoreGive(config_mutex);
    }
}

void config_store_get_saved(firmware_config_t *config)
{
    if (config == NULL || config_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(config_mutex, portMAX_DELAY) == pdTRUE) {
        *config = saved_config;
        xSemaphoreGive(config_mutex);
    }
}

esp_err_t config_store_update(
    const firmware_config_t *config,
    bool *changed)
{
    if (changed != NULL) {
        *changed = false;
    }
    if (config == NULL || config_mutex == NULL ||
        !config_model_validate(config, NULL, 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(config_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = ESP_OK;
    if (!config_model_mutable_equal(config, &saved_config)) {
        firmware_config_t candidate = *config;
        if (config_model_mutable_equal(&candidate, &active_config)) {
            candidate = active_config;
        } else {
            candidate.database_revision =
                saved_config.database_revision == UINT32_MAX
                ? 1U
                : saved_config.database_revision + 1U;
            config_model_finalize(&candidate);
        }
        result = write_saved_config(&candidate);
        if (result == ESP_OK) {
            saved_config = candidate;
            atomic_store_explicit(
                &restart_required,
                !config_model_mutable_equal(
                    &saved_config, &active_config),
                memory_order_release);
            if (changed != NULL) {
                *changed = true;
            }
        }
    }
    xSemaphoreGive(config_mutex);
    return result;
}

bool config_store_restart_required(void)
{
    return atomic_load_explicit(&restart_required, memory_order_acquire);
}
