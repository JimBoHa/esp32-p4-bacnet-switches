#include "ota_server.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_auth.h"
#include "sdkconfig.h"
#include "switch_inputs.h"

#define OTA_RECEIVE_BUFFER_BYTES 4096U
#define OTA_MAX_CONSECUTIVE_TIMEOUTS 5U
#define OTA_ROLLBACK_VALIDATION_DELAY_MS 10000U
#define OTA_PROJECT_HEADER "X-Firmware-Project"

static const char *TAG = "https_ota";

#if CONFIG_OTA_HTTPS_ENABLED

extern const unsigned char ota_server_cert_pem_start[]
    asm("_binary_ota_server_cert_pem_start");
extern const unsigned char ota_server_cert_pem_end[]
    asm("_binary_ota_server_cert_pem_end");
extern const unsigned char ota_server_key_pem_start[]
    asm("_binary_ota_server_key_pem_start");
extern const unsigned char ota_server_key_pem_end[]
    asm("_binary_ota_server_key_pem_end");

static httpd_handle_t server_handle;
static atomic_bool upload_in_progress;

static const char *json_bool(bool value)
{
    return value ? "true" : "false";
}

static bool response_append(
    char *response,
    size_t capacity,
    size_t *length,
    const char *format,
    ...)
{
    if (*length >= capacity) {
        return false;
    }

    va_list arguments;
    va_start(arguments, format);
    const int written = vsnprintf(
        response + *length,
        capacity - *length,
        format,
        arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= capacity - *length) {
        return false;
    }
    *length += (size_t)written;
    return true;
}

static bool append_pad_config(
    char *response,
    size_t capacity,
    size_t *length,
    const switch_input_pad_config_t *config)
{
    return response_append(
        response,
        capacity,
        length,
        "{\"valid\":%s,\"function_select\":%u,\"output_signal\":%u,"
        "\"drive_capability\":%u,\"pull_up\":%s,\"pull_down\":%s,"
        "\"input_enable\":%s,\"output_enable\":%s,"
        "\"oe_controlled_by_peripheral\":%s,\"oe_inverted\":%s,"
        "\"open_drain\":%s,\"sleep_select\":%s}",
        json_bool(config->valid),
        (unsigned)config->function_select,
        (unsigned)config->output_signal,
        (unsigned)config->drive_capability,
        json_bool(config->pull_up_enabled),
        json_bool(config->pull_down_enabled),
        json_bool(config->input_enabled),
        json_bool(config->output_enabled),
        json_bool(config->output_enable_controlled_by_peripheral),
        json_bool(config->output_enable_inverted),
        json_bool(config->open_drain_enabled),
        json_bool(config->sleep_select_enabled));
}

static size_t bounded_string_length(const char *value, size_t maximum)
{
    size_t length = 0;
    while (length < maximum && value[length] != '\0') {
        length++;
    }
    return length;
}

static esp_err_t send_unauthorized(httpd_req_t *request)
{
    httpd_resp_set_hdr(
        request, "WWW-Authenticate", "Bearer realm=\"ESP32 OTA\"");
    return httpd_resp_send_err(
        request, HTTPD_401_UNAUTHORIZED, "valid bearer token required");
}

static bool request_authenticated(httpd_req_t *request)
{
    const size_t length =
        httpd_req_get_hdr_value_len(request, "Authorization");
    if (length == 0U || length > OTA_AUTHORIZATION_MAX_LENGTH) {
        return false;
    }

    char authorization[OTA_AUTHORIZATION_MAX_LENGTH + 1U];
    if (httpd_req_get_hdr_value_str(
            request,
            "Authorization",
            authorization,
            sizeof(authorization)) != ESP_OK) {
        return false;
    }
    return ota_authorization_valid(
        authorization, length, CONFIG_OTA_BEARER_TOKEN);
}

static bool project_header_matches(httpd_req_t *request)
{
    const esp_app_desc_t *running = esp_app_get_description();
    const size_t length =
        httpd_req_get_hdr_value_len(request, OTA_PROJECT_HEADER);
    if (running == NULL || length == 0U ||
        length >= sizeof(running->project_name)) {
        return false;
    }

    char project[sizeof(running->project_name)];
    if (httpd_req_get_hdr_value_str(
            request, OTA_PROJECT_HEADER, project, sizeof(project)) != ESP_OK) {
        return false;
    }
    return bounded_string_length(
               running->project_name, sizeof(running->project_name)) == length &&
        memcmp(project, running->project_name, length) == 0;
}

static const char *ota_state_name(esp_ota_img_states_t state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW:
        return "new";
    case ESP_OTA_IMG_PENDING_VERIFY:
        return "pending-verify";
    case ESP_OTA_IMG_VALID:
        return "valid";
    case ESP_OTA_IMG_INVALID:
        return "invalid";
    case ESP_OTA_IMG_ABORTED:
        return "aborted";
    case ESP_OTA_IMG_UNDEFINED:
    default:
        return "undefined";
    }
}

static esp_err_t status_get_handler(httpd_req_t *request)
{
    if (!request_authenticated(request)) {
        return send_unauthorized(request);
    }

    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t image_state = ESP_OTA_IMG_UNDEFINED;
    if (running != NULL) {
        (void)esp_ota_get_state_partition(running, &image_state);
    }

    char response[4096];
    size_t response_length = 0;
    if (!response_append(
            response,
            sizeof(response),
            &response_length,
            "{\"ota\":true,\"project\":\"%.31s\",\"version\":\"%.31s\","
            "\"idf_version\":\"%.31s\",\"partition\":\"%.15s\","
            "\"state\":\"%s\",\"port\":%u,\"gpio_diagnostics\":[",
            app != NULL ? app->project_name : "unknown",
            app != NULL ? app->version : "unknown",
            esp_get_idf_version(),
            running != NULL ? running->label : "unknown",
            ota_state_name(image_state),
            (unsigned)CONFIG_OTA_HTTPS_PORT)) {
        return httpd_resp_send_err(
            request, HTTPD_500_INTERNAL_SERVER_ERROR, "status encoding failed");
    }

    for (size_t index = 0; index < SWITCH_INPUT_COUNT; ++index) {
        switch_input_diagnostics_t diagnostics;
        if (!switch_input_diagnostics_get(index, &diagnostics) ||
            !response_append(
                response,
                sizeof(response),
                &response_length,
                "%s{\"gpio\":%d,\"startup\":{"
                "\"raw_after_input_enable\":%s,\"config\":",
                index == 0 ? "" : ",",
                diagnostics.gpio,
                json_bool(diagnostics.startup_raw_after_input_enable)) ||
            !append_pad_config(
                response,
                sizeof(response),
                &response_length,
                &diagnostics.startup_config) ||
            !response_append(
                response,
                sizeof(response),
                &response_length,
                "},\"configured\":{\"raw\":%s,\"config\":",
                json_bool(diagnostics.configured_raw)) ||
            !append_pad_config(
                response,
                sizeof(response),
                &response_length,
                &diagnostics.configured_config) ||
            !response_append(
                response,
                sizeof(response),
                &response_length,
                "},\"current\":{\"raw\":%s,\"stable\":%s,\"config\":",
                json_bool(diagnostics.current_raw),
                json_bool(diagnostics.stable)) ||
            !append_pad_config(
                response,
                sizeof(response),
                &response_length,
                &diagnostics.current_config) ||
            !response_append(
                response,
                sizeof(response),
                &response_length,
                "}}")) {
            return httpd_resp_send_err(
                request,
                HTTPD_500_INTERNAL_SERVER_ERROR,
                "GPIO diagnostics encoding failed");
        }
    }
    if (!response_append(
            response,
            sizeof(response),
            &response_length,
            "]}")) {
        return httpd_resp_send_err(
            request, HTTPD_500_INTERNAL_SERVER_ERROR, "status encoding failed");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, response, response_length);
}

static esp_err_t ota_post_handler(httpd_req_t *request)
{
    if (!request_authenticated(request)) {
        return send_unauthorized(request);
    }
    if (!project_header_matches(request)) {
        return httpd_resp_send_err(
            request,
            HTTPD_400_BAD_REQUEST,
            "missing or incorrect X-Firmware-Project header");
    }
    if (atomic_exchange_explicit(
            &upload_in_progress, true, memory_order_acq_rel)) {
        httpd_resp_set_status(request, "409 Conflict");
        return httpd_resp_sendstr(request, "OTA update already in progress");
    }

    esp_err_t result = ESP_FAIL;
    httpd_err_code_t http_error = HTTPD_500_INTERNAL_SERVER_ERROR;
    const char *error_message = "OTA update failed";
    uint8_t *buffer = NULL;
    esp_ota_handle_t ota_handle = 0;
    bool ota_started = false;

    const esp_partition_t *update_partition =
        esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        error_message = "no inactive OTA partition";
        goto fail;
    }
    if (request->content_len == 0U) {
        http_error = HTTPD_411_LENGTH_REQUIRED;
        error_message = "firmware Content-Length required";
        goto fail;
    }
    if (request->content_len > update_partition->size) {
        http_error = HTTPD_413_CONTENT_TOO_LARGE;
        error_message = "firmware image exceeds OTA partition";
        goto fail;
    }

    buffer = malloc(OTA_RECEIVE_BUFFER_BYTES);
    if (buffer == NULL) {
        error_message = "not enough memory for OTA buffer";
        goto fail;
    }
    result = esp_ota_begin(
        update_partition, request->content_len, &ota_handle);
    if (result != ESP_OK) {
        error_message = "could not begin OTA write";
        goto fail;
    }
    ota_started = true;

    ESP_LOGI(
        TAG,
        "receiving %u bytes into %s",
        (unsigned)request->content_len,
        update_partition->label);
    size_t remaining = request->content_len;
    unsigned consecutive_timeouts = 0;
    while (remaining > 0U) {
        const size_t wanted = remaining < OTA_RECEIVE_BUFFER_BYTES
            ? remaining
            : OTA_RECEIVE_BUFFER_BYTES;
        const int received =
            httpd_req_recv(request, (char *)buffer, wanted);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            consecutive_timeouts++;
            if (consecutive_timeouts <= OTA_MAX_CONSECUTIVE_TIMEOUTS) {
                continue;
            }
            http_error = HTTPD_408_REQ_TIMEOUT;
            error_message = "firmware upload timed out";
            goto fail;
        }
        if (received <= 0) {
            http_error = HTTPD_400_BAD_REQUEST;
            error_message = "firmware upload ended early";
            goto fail;
        }
        consecutive_timeouts = 0;
        result = esp_ota_write(ota_handle, buffer, (size_t)received);
        if (result != ESP_OK) {
            error_message = "firmware flash write failed";
            goto fail;
        }
        remaining -= (size_t)received;
    }
    free(buffer);
    buffer = NULL;

    result = esp_ota_end(ota_handle);
    ota_started = false;
    if (result != ESP_OK) {
        error_message = "firmware image validation failed";
        goto fail;
    }

    esp_app_desc_t candidate;
    result = esp_ota_get_partition_description(update_partition, &candidate);
    if (result != ESP_OK) {
        error_message = "firmware description unreadable";
        goto fail;
    }
    const esp_app_desc_t *running = esp_app_get_description();
    if (running == NULL ||
        strncmp(
            candidate.project_name,
            running->project_name,
            sizeof(candidate.project_name)) != 0) {
        http_error = HTTPD_400_BAD_REQUEST;
        error_message = "firmware belongs to a different project";
        goto fail;
    }

    result = esp_ota_set_boot_partition(update_partition);
    if (result != ESP_OK) {
        error_message = "could not select new boot partition";
        goto fail;
    }

    char response[192];
    const int response_length = snprintf(
        response,
        sizeof(response),
        "{\"accepted\":true,\"version\":\"%.31s\","
        "\"partition\":\"%.15s\",\"rebooting\":true}",
        candidate.version,
        update_partition->label);
    httpd_resp_set_status(request, "202 Accepted");
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (response_length > 0 && (size_t)response_length < sizeof(response)) {
        (void)httpd_resp_send(request, response, response_length);
    } else {
        (void)httpd_resp_sendstr(request, "update accepted; rebooting");
    }
    ESP_LOGI(
        TAG,
        "accepted version %.31s; rebooting into %s",
        candidate.version,
        update_partition->label);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;

fail:
    if (ota_started) {
        (void)esp_ota_abort(ota_handle);
    }
    free(buffer);
    atomic_store_explicit(
        &upload_in_progress, false, memory_order_release);
    ESP_LOGE(
        TAG,
        "%s: %s",
        error_message,
        esp_err_to_name(result));
    return httpd_resp_send_err(request, http_error, error_message);
}

esp_err_t ota_server_start(void)
{
    if (server_handle != NULL) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(
        ota_token_configuration_valid(CONFIG_OTA_BEARER_TOKEN),
        ESP_ERR_INVALID_ARG,
        TAG,
        "OTA bearer token must contain 32-128 characters");

    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    config.port_secure = CONFIG_OTA_HTTPS_PORT;
    config.httpd.max_uri_handlers = 2;
    config.httpd.max_open_sockets = 2;
    config.httpd.stack_size = 12288;
    config.servercert = ota_server_cert_pem_start;
    config.servercert_len =
        ota_server_cert_pem_end - ota_server_cert_pem_start;
    config.prvtkey_pem = ota_server_key_pem_start;
    config.prvtkey_len =
        ota_server_key_pem_end - ota_server_key_pem_start;

    httpd_handle_t created_server = NULL;
    ESP_RETURN_ON_ERROR(
        httpd_ssl_start(&created_server, &config),
        TAG,
        "failed to start HTTPS OTA server");

    const httpd_uri_t status_uri = {
        .uri = "/ota/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
    };
    const httpd_uri_t update_uri = {
        .uri = "/ota",
        .method = HTTP_POST,
        .handler = ota_post_handler,
    };
    esp_err_t result =
        httpd_register_uri_handler(created_server, &status_uri);
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(created_server, &update_uri);
    }
    if (result != ESP_OK) {
        (void)httpd_ssl_stop(created_server);
        return result;
    }

    atomic_store_explicit(
        &upload_in_progress, false, memory_order_release);
    server_handle = created_server;
    ESP_LOGI(
        TAG,
        "authenticated HTTPS OTA ready on port %u",
        (unsigned)CONFIG_OTA_HTTPS_PORT);
    return ESP_OK;
}

#else

esp_err_t ota_server_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
static void rollback_validation_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(OTA_ROLLBACK_VALIDATION_DELAY_MS));

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running != NULL &&
        esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        const esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
        if (result == ESP_OK) {
            ESP_LOGI(TAG, "running firmware marked valid");
        } else {
            ESP_LOGE(
                TAG,
                "failed to confirm running firmware: %s",
                esp_err_to_name(result));
        }
    }
    vTaskDelete(NULL);
}
#endif

esp_err_t ota_start_rollback_validation(void)
{
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running == NULL ||
        esp_ota_get_state_partition(running, &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return ESP_OK;
    }

    const BaseType_t created = xTaskCreate(
        rollback_validation_task,
        "ota_validate",
        3072,
        NULL,
        4,
        NULL);
    ESP_RETURN_ON_FALSE(
        created == pdPASS,
        ESP_ERR_NO_MEM,
        TAG,
        "failed to create OTA validation task");
    ESP_LOGI(
        TAG,
        "new firmware pending; validation in %u ms",
        OTA_ROLLBACK_VALIDATION_DELAY_MS);
#endif
    return ESP_OK;
}
