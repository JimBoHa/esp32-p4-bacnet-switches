#include "network_config_store.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#define NETWORK_NVS_NAMESPACE "p4_network"
#define NETWORK_NVS_CONFIRMED_KEY "confirmed"
#define NETWORK_NVS_PENDING_KEY "pending"
#define NETWORK_NVS_TRIAL_BOOT_KEY "trial_boot"

static const char *TAG = "network_config";
static SemaphoreHandle_t config_mutex;
static network_config_t active_config;
static network_config_t saved_config;
static network_config_t confirmed_config;
static atomic_bool pending_present;
static atomic_bool trial_active;
static uint64_t trial_deadline_ms;
static portMUX_TYPE trial_time_lock = portMUX_INITIALIZER_UNLOCKED;

static void factory_defaults(network_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->revision = 1U;
    config->mode = NETWORK_ADDRESS_DHCP;
    (void)snprintf(
        config->hostname,
        sizeof(config->hostname),
        "%s",
        CONFIG_BACNET_HOSTNAME);
    network_config_finalize(config);
}

static bool read_blob(const char *key, network_config_t *config)
{
    nvs_handle_t handle;
    if (nvs_open(NETWORK_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t size = sizeof(*config);
    const esp_err_t result = nvs_get_blob(handle, key, config, &size);
    nvs_close(handle);
    return result == ESP_OK && size == sizeof(*config) &&
        network_config_is_valid_blob(config);
}

static esp_err_t write_single_blob(
    const char *key,
    const network_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(
        NETWORK_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    result = nvs_set_blob(handle, key, config, sizeof(*config));
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

static esp_err_t stage_pending(const network_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(
        NETWORK_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    result = nvs_set_blob(
        handle, NETWORK_NVS_PENDING_KEY, config, sizeof(*config));
    if (result == ESP_OK) {
        result = nvs_erase_key(handle, NETWORK_NVS_TRIAL_BOOT_KEY);
        if (result == ESP_ERR_NVS_NOT_FOUND) {
            result = ESP_OK;
        }
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

static bool trial_boot_attempted(void)
{
    nvs_handle_t handle;
    if (nvs_open(NETWORK_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    uint8_t attempted = 0U;
    const esp_err_t result = nvs_get_u8(
        handle, NETWORK_NVS_TRIAL_BOOT_KEY, &attempted);
    nvs_close(handle);
    return result == ESP_OK && attempted == 1U;
}

static esp_err_t mark_trial_boot_attempted(void)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(
        NETWORK_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    result = nvs_set_u8(handle, NETWORK_NVS_TRIAL_BOOT_KEY, 1U);
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

static esp_err_t erase_pending(void)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(
        NETWORK_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    result = nvs_erase_key(handle, NETWORK_NVS_PENDING_KEY);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        result = ESP_OK;
    }
    if (result == ESP_OK) {
        result = nvs_erase_key(handle, NETWORK_NVS_TRIAL_BOOT_KEY);
        if (result == ESP_ERR_NVS_NOT_FOUND) {
            result = ESP_OK;
        }
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

static esp_err_t promote_active(void)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(
        NETWORK_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    result = nvs_set_blob(
        handle,
        NETWORK_NVS_CONFIRMED_KEY,
        &active_config,
        sizeof(active_config));
    if (result == ESP_OK) {
        result = nvs_erase_key(handle, NETWORK_NVS_PENDING_KEY);
        if (result == ESP_ERR_NVS_NOT_FOUND) {
            result = ESP_OK;
        }
    }
    if (result == ESP_OK) {
        result = nvs_erase_key(handle, NETWORK_NVS_TRIAL_BOOT_KEY);
        if (result == ESP_ERR_NVS_NOT_FOUND) {
            result = ESP_OK;
        }
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

esp_err_t network_config_store_init(void)
{
    config_mutex = xSemaphoreCreateMutex();
    if (config_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (!read_blob(NETWORK_NVS_CONFIRMED_KEY, &confirmed_config)) {
        factory_defaults(&confirmed_config);
        if (!network_config_validate(&confirmed_config, NULL, 0U)) {
            return ESP_ERR_INVALID_ARG;
        }
        const esp_err_t result = write_single_blob(
            NETWORK_NVS_CONFIRMED_KEY, &confirmed_config);
        if (result != ESP_OK) {
            return result;
        }
        ESP_LOGW(TAG, "installed DHCP factory network configuration");
    }

    bool pending = read_blob(
        NETWORK_NVS_PENDING_KEY, &saved_config) &&
        !network_config_mutable_equal(&saved_config, &confirmed_config);
    if (pending && trial_boot_attempted()) {
        ESP_LOGW(
            TAG,
            "previous network trial did not complete; restoring confirmed revision %u",
            (unsigned)confirmed_config.revision);
        const esp_err_t rollback_result = erase_pending();
        if (rollback_result != ESP_OK) {
            return rollback_result;
        }
        pending = false;
    }
    if (pending) {
        const esp_err_t marker_result = mark_trial_boot_attempted();
        if (marker_result != ESP_OK) {
            const esp_err_t rollback_result = erase_pending();
            if (rollback_result != ESP_OK) {
                return rollback_result;
            }
            pending = false;
            ESP_LOGE(
                TAG,
                "could not arm safe network trial; discarded pending configuration");
        }
    }
    if (!pending) {
        saved_config = confirmed_config;
        (void)erase_pending();
    }
    active_config = pending ? saved_config : confirmed_config;
    atomic_init(&pending_present, pending);
    atomic_init(&trial_active, pending);
    trial_deadline_ms = 0U;
    ESP_LOGI(
        TAG,
        "using %s network configuration revision %u%s",
        active_config.mode == NETWORK_ADDRESS_DHCP ? "DHCP" : "static",
        (unsigned)active_config.revision,
        pending ? " as an unconfirmed trial" : "");
    return ESP_OK;
}

static void copy_config(const network_config_t *source, network_config_t *target)
{
    if (target == NULL || config_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(config_mutex, portMAX_DELAY) == pdTRUE) {
        *target = *source;
        xSemaphoreGive(config_mutex);
    }
}

void network_config_get_active(network_config_t *config)
{
    copy_config(&active_config, config);
}

void network_config_get_saved(network_config_t *config)
{
    copy_config(&saved_config, config);
}

void network_config_get_confirmed(network_config_t *config)
{
    copy_config(&confirmed_config, config);
}

esp_err_t network_config_update(
    const network_config_t *config,
    bool *changed)
{
    if (changed != NULL) {
        *changed = false;
    }
    if (config == NULL || config_mutex == NULL ||
        !network_config_validate(config, NULL, 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_load_explicit(&trial_active, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(config_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = ESP_OK;
    if (!network_config_mutable_equal(config, &saved_config)) {
        if (network_config_mutable_equal(config, &confirmed_config)) {
            result = erase_pending();
            if (result == ESP_OK) {
                atomic_store_explicit(
                    &pending_present, false, memory_order_release);
                saved_config = confirmed_config;
            }
        } else {
            network_config_t candidate = *config;
            candidate.revision = confirmed_config.revision == UINT32_MAX
                ? 1U
                : confirmed_config.revision + 1U;
            network_config_finalize(&candidate);
            result = stage_pending(&candidate);
            if (result == ESP_OK) {
                atomic_store_explicit(
                    &pending_present, true, memory_order_release);
                saved_config = candidate;
            }
        }
        if (result == ESP_OK && changed != NULL) {
            *changed = true;
        }
    }
    xSemaphoreGive(config_mutex);
    return result;
}

bool network_config_restart_required(void)
{
    return atomic_load_explicit(&pending_present, memory_order_acquire) &&
        !atomic_load_explicit(&trial_active, memory_order_acquire);
}

bool network_config_trial_active(void)
{
    return atomic_load_explicit(&trial_active, memory_order_acquire);
}

uint32_t network_config_trial_seconds_remaining(void)
{
    if (!network_config_trial_active()) {
        return 0U;
    }
    portENTER_CRITICAL(&trial_time_lock);
    const uint64_t deadline = trial_deadline_ms;
    portEXIT_CRITICAL(&trial_time_lock);
    const uint64_t now = (uint64_t)esp_timer_get_time() / 1000U;
    if (deadline <= now) {
        return 0U;
    }
    const uint64_t remaining_ms = deadline - now;
    return (uint32_t)((remaining_ms + 999U) / 1000U);
}

esp_err_t network_config_confirm_trial(void)
{
    if (config_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(config_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!network_config_trial_active()) {
        xSemaphoreGive(config_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = promote_active();
    if (result == ESP_OK) {
        confirmed_config = active_config;
        saved_config = active_config;
        atomic_store_explicit(
            &pending_present, false, memory_order_release);
        atomic_store_explicit(&trial_active, false, memory_order_release);
        ESP_LOGI(
            TAG,
            "confirmed network configuration revision %u",
            (unsigned)active_config.revision);
    }
    xSemaphoreGive(config_mutex);
    return result;
}

static void trial_guard_task(void *argument)
{
    (void)argument;
    while (network_config_trial_active()) {
        if (network_config_trial_seconds_remaining() > 0U) {
            vTaskDelay(pdMS_TO_TICKS(1000U));
            continue;
        }
        if (xSemaphoreTake(config_mutex, portMAX_DELAY) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(1000U));
            continue;
        }
        if (!network_config_trial_active()) {
            xSemaphoreGive(config_mutex);
            break;
        }
        const esp_err_t result = erase_pending();
        if (result == ESP_OK) {
            atomic_store_explicit(
                &pending_present, false, memory_order_release);
            saved_config = confirmed_config;
            atomic_store_explicit(
                &trial_active, false, memory_order_release);
        }
        xSemaphoreGive(config_mutex);
        if (result == ESP_OK) {
            ESP_LOGW(
                TAG,
                "network trial was not confirmed; rebooting to revision %u",
                (unsigned)confirmed_config.revision);
            vTaskDelay(pdMS_TO_TICKS(500U));
            esp_restart();
        }
        ESP_LOGE(
            TAG,
            "could not roll back unconfirmed network configuration: %s",
            esp_err_to_name(result));
        vTaskDelay(pdMS_TO_TICKS(5000U));
    }
    vTaskDelete(NULL);
}

esp_err_t network_config_start_trial_guard(void)
{
    if (!network_config_trial_active()) {
        return ESP_OK;
    }
    const uint64_t now = (uint64_t)esp_timer_get_time() / 1000U;
    portENTER_CRITICAL(&trial_time_lock);
    trial_deadline_ms = now +
        (uint64_t)NETWORK_CONFIG_TRIAL_TIMEOUT_SECONDS * 1000U;
    portEXIT_CRITICAL(&trial_time_lock);
    const BaseType_t result = xTaskCreate(
        trial_guard_task,
        "network_trial",
        4096,
        NULL,
        4,
        NULL);
    return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
