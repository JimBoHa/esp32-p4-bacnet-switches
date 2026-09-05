#include "discovery_service.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "mdns.h"

#define DISCOVERY_INSTANCE_NAME_MAX_BYTES 64U
#define DISCOVERY_VERSION_MAX_BYTES 32U

static const char *TAG = "discovery";
static discovery_service_snapshot_t service_state;
static portMUX_TYPE service_state_lock = portMUX_INITIALIZER_UNLOCKED;

static void hostname_changed(const char *hostname, void *argument)
{
    (void)argument;
    portENTER_CRITICAL(&service_state_lock);
    (void)snprintf(
        service_state.hostname,
        sizeof(service_state.hostname),
        "%s",
        hostname);
    service_state.hostname_conflict_count++;
    portEXIT_CRITICAL(&service_state_lock);
    ESP_LOGW(TAG, "mDNS hostname changed after a conflict: %s.local", hostname);
}

static esp_err_t validate_arguments(
    const char *hostname,
    const char *instance_name,
    uint16_t bacnet_port,
    const char *firmware_version)
{
    if (hostname == NULL || instance_name == NULL || firmware_version == NULL ||
        hostname[0] == '\0' || instance_name[0] == '\0' ||
        firmware_version[0] == '\0' || bacnet_port == 0U ||
        strnlen(hostname, NETWORK_CONFIG_HOSTNAME_LENGTH) >=
            NETWORK_CONFIG_HOSTNAME_LENGTH ||
        strnlen(instance_name, DISCOVERY_INSTANCE_NAME_MAX_BYTES) >=
            DISCOVERY_INSTANCE_NAME_MAX_BYTES ||
        strnlen(firmware_version, DISCOVERY_VERSION_MAX_BYTES) >=
            DISCOVERY_VERSION_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t fail_and_stop(esp_err_t result)
{
    mdns_free();
    service_state.ready = false;
    service_state.https_advertised = false;
    service_state.bacnet_advertised = false;
    service_state.last_error = result;
    ESP_LOGE(TAG, "mDNS discovery failed: %s", esp_err_to_name(result));
    return result;
}

esp_err_t discovery_service_start(
    const char *hostname,
    const char *instance_name,
    uint16_t https_port,
    uint16_t bacnet_port,
    uint32_t device_instance,
    uint16_t vendor_identifier,
    const char *firmware_version)
{
    memset(&service_state, 0, sizeof(service_state));
    const esp_err_t argument_result = validate_arguments(
        hostname, instance_name, bacnet_port, firmware_version);
    if (argument_result != ESP_OK) {
        service_state.last_error = argument_result;
        return argument_result;
    }

    (void)snprintf(
        service_state.hostname,
        sizeof(service_state.hostname),
        "%s",
        hostname);
    service_state.https_port = https_port;
    service_state.bacnet_port = bacnet_port;

    esp_err_t result = mdns_init();
    if (result != ESP_OK) {
        service_state.last_error = result;
        ESP_LOGE(TAG, "mDNS initialization failed: %s", esp_err_to_name(result));
        return result;
    }
    result = mdns_hostname_set(hostname);
    if (result != ESP_OK) {
        return fail_and_stop(result);
    }
    result = mdns_register_hostname_changed_callback(hostname_changed, NULL);
    if (result != ESP_OK) {
        return fail_and_stop(result);
    }
    result = mdns_instance_name_set(instance_name);
    if (result != ESP_OK) {
        return fail_and_stop(result);
    }

    const mdns_txt_item_t common_txt[] = {
        {"project", "esp32_p4_bacnet_switches"},
        {"version", firmware_version},
    };
    if (https_port != 0U) {
        mdns_txt_item_t https_txt[] = {
            {"path", "/ota/status"},
            {"auth", "bearer"},
            common_txt[0],
            common_txt[1],
        };
        result = mdns_service_add(
            instance_name,
            "_https",
            "_tcp",
            https_port,
            https_txt,
            sizeof(https_txt) / sizeof(https_txt[0]));
        if (result != ESP_OK) {
            return fail_and_stop(result);
        }
        service_state.https_advertised = true;
    }

    char device_instance_text[11];
    char vendor_identifier_text[6];
    (void)snprintf(
        device_instance_text,
        sizeof(device_instance_text),
        "%lu",
        (unsigned long)device_instance);
    (void)snprintf(
        vendor_identifier_text,
        sizeof(vendor_identifier_text),
        "%u",
        (unsigned)vendor_identifier);
    mdns_txt_item_t bacnet_txt[] = {
        {"device", device_instance_text},
        {"vendor", vendor_identifier_text},
        common_txt[0],
        common_txt[1],
    };
    result = mdns_service_add(
        instance_name,
        "_bacnet",
        "_udp",
        bacnet_port,
        bacnet_txt,
        sizeof(bacnet_txt) / sizeof(bacnet_txt[0]));
    if (result != ESP_OK) {
        return fail_and_stop(result);
    }
    service_state.bacnet_advertised = true;
    service_state.ready = true;
    service_state.last_error = ESP_OK;
    ESP_LOGI(
        TAG,
        "advertising %s.local: HTTPS=%u, BACnet/IP=%u",
        hostname,
        (unsigned)https_port,
        (unsigned)bacnet_port);
    return ESP_OK;
}

bool discovery_service_snapshot_get(discovery_service_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return false;
    }
    portENTER_CRITICAL(&service_state_lock);
    *snapshot = service_state;
    portEXIT_CRITICAL(&service_state_lock);
    return true;
}
