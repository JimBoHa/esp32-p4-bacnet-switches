#include "ota_server.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bacnet_server.h"
#include "diagnostics.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_auth.h"
#include "ota_health.h"
#include "sdkconfig.h"
#include "switch_inputs.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"

#define OTA_RECEIVE_BUFFER_BYTES 4096U
#define OTA_MAX_CONSECUTIVE_TIMEOUTS 5U
#define OTA_ROLLBACK_VALIDATION_INITIAL_DELAY_MS 10000U
#define OTA_ROLLBACK_VALIDATION_POLL_MS 1000U
#define OTA_ROLLBACK_VALIDATION_TIMEOUT_MS 60000U
#define OTA_ROLLBACK_HEALTHY_SAMPLES 5U
#define OTA_PROJECT_HEADER "X-Firmware-Project"
#define OTA_STATUS_RESPONSE_BYTES 12288U
#define OTA_SHA256_BYTES 32U
#define OTA_SHA256_HEX_BYTES (OTA_SHA256_BYTES * 2U)

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
extern const unsigned char ota_token_txt_start[]
    asm("_binary_ota_token_txt_start");
extern const unsigned char ota_token_txt_end[]
    asm("_binary_ota_token_txt_end");

static httpd_handle_t server_handle;
static atomic_bool upload_in_progress;
static atomic_bool server_ready;
static char bearer_token[OTA_TOKEN_MAX_LENGTH + 1U];
static char running_image_sha256[OTA_SHA256_HEX_BYTES + 1U];

static bool load_embedded_token(void)
{
    return ota_copy_embedded_token(
        ota_token_txt_start,
        (size_t)(ota_token_txt_end - ota_token_txt_start),
        bearer_token,
        sizeof(bearer_token));
}

static void sha256_to_hex(
    const uint8_t digest[OTA_SHA256_BYTES],
    char hexadecimal[OTA_SHA256_HEX_BYTES + 1U])
{
    static const char DIGITS[] = "0123456789abcdef";
    for (size_t index = 0; index < OTA_SHA256_BYTES; ++index) {
        hexadecimal[index * 2U] = DIGITS[digest[index] >> 4U];
        hexadecimal[index * 2U + 1U] = DIGITS[digest[index] & 0x0FU];
    }
    hexadecimal[OTA_SHA256_HEX_BYTES] = '\0';
}

static int mbedtls_hardware_random(
    void *context,
    unsigned char *output,
    size_t output_length)
{
    (void)context;
    esp_fill_random(output, output_length);
    return 0;
}

static esp_err_t validate_embedded_tls_credentials(void)
{
    mbedtls_x509_crt certificate;
    mbedtls_pk_context private_key;
    mbedtls_x509_crt_init(&certificate);
    mbedtls_pk_init(&private_key);

    int result = mbedtls_x509_crt_parse(
        &certificate,
        ota_server_cert_pem_start,
        (size_t)(ota_server_cert_pem_end - ota_server_cert_pem_start));
    if (result == 0) {
        result = mbedtls_pk_parse_key(
            &private_key,
            ota_server_key_pem_start,
            (size_t)(ota_server_key_pem_end - ota_server_key_pem_start),
            NULL,
            0U,
            mbedtls_hardware_random,
            NULL);
    }
    if (result == 0) {
        result = mbedtls_pk_check_pair(
            &certificate.pk,
            &private_key,
            mbedtls_hardware_random,
            NULL);
    }

    mbedtls_pk_free(&private_key);
    mbedtls_x509_crt_free(&certificate);
    if (result != 0) {
        const unsigned error_code =
            result < 0 ? (unsigned)(-result) : (unsigned)result;
        ESP_LOGE(
            TAG,
            "embedded TLS certificate/key preflight failed: -0x%04X",
            error_code);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t cache_running_image_sha256(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    uint8_t digest[OTA_SHA256_BYTES];
    ESP_RETURN_ON_FALSE(
        running != NULL,
        ESP_ERR_NOT_FOUND,
        TAG,
        "running OTA partition not found");
    ESP_RETURN_ON_ERROR(
        esp_partition_get_sha256(running, digest),
        TAG,
        "running firmware image hash validation failed");
    sha256_to_hex(digest, running_image_sha256);
    return ESP_OK;
}

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
        authorization, length, bearer_token);
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

static bool running_image_allows_update(
    httpd_req_t *request,
    esp_err_t *response_result)
{
    *response_result = ESP_OK;
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running == NULL) {
        *response_result = httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "running OTA partition unavailable");
        return false;
    }
    const esp_err_t state_result =
        esp_ota_get_state_partition(running, &state);
    if (state_result == ESP_ERR_NOT_FOUND) {
        return true;
    }
    if (state_result != ESP_OK) {
        *response_result = httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "running OTA state unavailable");
        return false;
    }
    if (state == ESP_OTA_IMG_NEW ||
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        httpd_resp_set_status(request, "409 Conflict");
        *response_result = httpd_resp_sendstr(
            request,
            "running firmware is not validated; retry after status is valid");
        return false;
    }
#else
    (void)request;
#endif
    return true;
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

static const char *dhcp_status_name(uint32_t status)
{
    switch ((esp_netif_dhcp_status_t)status) {
    case ESP_NETIF_DHCP_STARTED:
        return "started";
    case ESP_NETIF_DHCP_STOPPED:
        return "stopped";
    case ESP_NETIF_DHCP_INIT:
    default:
        return "initializing";
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
    diagnostics_snapshot_t snapshot;
    if (!diagnostics_snapshot_get(&snapshot)) {
        return httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "diagnostics snapshot failed");
    }

    char *response = malloc(OTA_STATUS_RESPONSE_BYTES);
    if (response == NULL) {
        return httpd_resp_send_err(
            request, HTTPD_500_INTERNAL_SERVER_ERROR, "not enough memory");
    }
    size_t response_length = 0;
    if (!response_append(
            response,
            OTA_STATUS_RESPONSE_BYTES,
            &response_length,
            "{\"ota\":true,\"project\":\"%.31s\",\"version\":\"%.31s\","
            "\"idf_version\":\"%.31s\",\"partition\":\"%.15s\","
            "\"state\":\"%s\",\"port\":%u,\"git_revision\":\"%.31s\","
            "\"image_sha256\":\"%s\","
            "\"system\":{\"uptime_ms\":%u,\"chip_temperature_c\":",
            app != NULL ? app->project_name : "unknown",
            app != NULL ? app->version : "unknown",
            esp_get_idf_version(),
            running != NULL ? running->label : "unknown",
            ota_state_name(image_state),
            (unsigned)CONFIG_OTA_HTTPS_PORT,
            diagnostics_git_revision(),
            running_image_sha256,
            (unsigned)snapshot.uptime_ms)) {
        goto encoding_failed;
    }
    if (!response_append(
            response,
            OTA_STATUS_RESPONSE_BYTES,
            &response_length,
            snapshot.chip_temperature_valid ? "%.2f" : "null",
            (double)snapshot.chip_temperature_c) ||
        !response_append(
            response,
            OTA_STATUS_RESPONSE_BYTES,
            &response_length,
            ",\"free_heap_bytes\":%u,\"minimum_free_heap_bytes\":%u,"
            "\"reset_reason\":{\"code\":%u,\"name\":\"%s\"},"
            "\"boot_count\":%u,\"last_ota_result\":\"%s\","
            "\"task_watchdog\":{"
            "\"switch_inputs\":{\"subscribed\":%s,\"healthy\":%s,"
            "\"last_heartbeat_ms\":%u},"
            "\"bacnet\":{\"subscribed\":%s,\"healthy\":%s,"
            "\"last_heartbeat_ms\":%u}}},",
            (unsigned)snapshot.free_heap_bytes,
            (unsigned)snapshot.minimum_free_heap_bytes,
            (unsigned)snapshot.reset_reason,
            diagnostics_reset_reason_name(snapshot.reset_reason),
            (unsigned)snapshot.boot_count,
            diagnostics_ota_result_name(snapshot.last_ota_result),
            json_bool(snapshot.task_watchdog_subscribed[
                DIAGNOSTICS_TASK_SWITCH_INPUTS]),
            json_bool(snapshot.task_healthy[DIAGNOSTICS_TASK_SWITCH_INPUTS]),
            (unsigned)snapshot.task_last_heartbeat_ms[
                DIAGNOSTICS_TASK_SWITCH_INPUTS],
            json_bool(snapshot.task_watchdog_subscribed[
                DIAGNOSTICS_TASK_BACNET]),
            json_bool(snapshot.task_healthy[DIAGNOSTICS_TASK_BACNET]),
            (unsigned)snapshot.task_last_heartbeat_ms[
                DIAGNOSTICS_TASK_BACNET])) {
        goto encoding_failed;
    }

    const esp_ip4_addr_t ipv4 = {.addr = snapshot.network.ipv4_address};
    const esp_ip4_addr_t netmask = {.addr = snapshot.network.ipv4_netmask};
    const esp_ip4_addr_t gateway = {.addr = snapshot.network.ipv4_gateway};
    const uint32_t address_age_ms = snapshot.network.ipv4_address != 0U
        ? snapshot.uptime_ms - snapshot.network.ip_acquired_uptime_ms
        : 0U;
    if (!response_append(
            response,
            OTA_STATUS_RESPONSE_BYTES,
            &response_length,
            "\"network\":{\"link_up\":%s,\"speed_mbps\":%u,"
            "\"full_duplex\":%s,\"autonegotiation\":%s,"
            "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
            "\"link_up_count\":%u,\"link_down_count\":%u,"
            "\"reconnect_count\":%u,\"ip_acquisition_count\":%u,"
            "\"ip_changed_count\":%u,\"ipv4\":\"" IPSTR "\","
            "\"netmask\":\"" IPSTR "\",\"gateway\":\"" IPSTR "\","
            "\"dhcp_status\":\"%s\",\"ip_acquired_uptime_ms\":%u,"
            "\"address_age_ms\":%u},",
            json_bool(snapshot.network.link_up),
            (unsigned)snapshot.network.speed_mbps,
            json_bool(snapshot.network.full_duplex),
            json_bool(snapshot.network.autonegotiation),
            snapshot.network.mac[0],
            snapshot.network.mac[1],
            snapshot.network.mac[2],
            snapshot.network.mac[3],
            snapshot.network.mac[4],
            snapshot.network.mac[5],
            (unsigned)snapshot.network.link_up_count,
            (unsigned)snapshot.network.link_down_count,
            (unsigned)snapshot.network.reconnect_count,
            (unsigned)snapshot.network.ip_acquisition_count,
            (unsigned)snapshot.network.ip_changed_count,
            IP2STR(&ipv4),
            IP2STR(&netmask),
            IP2STR(&gateway),
            dhcp_status_name(snapshot.network.dhcp_status),
            (unsigned)snapshot.network.ip_acquired_uptime_ms,
            (unsigned)address_age_ms) ||
        !response_append(
            response,
            OTA_STATUS_RESPONSE_BYTES,
            &response_length,
            "\"bacnet\":{\"rx\":%u,\"who_is\":%u,"
            "\"read_property\":%u,\"read_property_multiple\":%u,"
            "\"subscribe_cov\":%u,\"malformed\":%u,\"ignored\":%u,"
            "\"responses\":%u,\"errors\":%u,\"rate_limited\":%u,"
            "\"cov_sent\":%u,\"cov_acked\":%u,\"cov_timeouts\":%u,"
            "\"active_cov_subscriptions\":%u},\"gpio_diagnostics\":[",
            (unsigned)snapshot.bacnet[DIAGNOSTICS_BACNET_RX],
            (unsigned)snapshot.bacnet[DIAGNOSTICS_BACNET_WHO_IS],
            (unsigned)snapshot.bacnet[DIAGNOSTICS_BACNET_READ_PROPERTY],
            (unsigned)snapshot.bacnet[
                DIAGNOSTICS_BACNET_READ_PROPERTY_MULTIPLE],
            (unsigned)snapshot.bacnet[DIAGNOSTICS_BACNET_SUBSCRIBE_COV],
            (unsigned)snapshot.bacnet[DIAGNOSTICS_BACNET_MALFORMED],
            (unsigned)snapshot.bacnet[DIAGNOSTICS_BACNET_IGNORED],
            (unsigned)snapshot.bacnet[DIAGNOSTICS_BACNET_RESPONSES],
            (unsigned)snapshot.bacnet[DIAGNOSTICS_BACNET_ERRORS],
            (unsigned)snapshot.bacnet[DIAGNOSTICS_BACNET_RATE_LIMITED],
            (unsigned)snapshot.bacnet[DIAGNOSTICS_BACNET_COV_SENT],
            (unsigned)snapshot.bacnet[DIAGNOSTICS_BACNET_COV_ACKED],
            (unsigned)snapshot.bacnet[DIAGNOSTICS_BACNET_COV_TIMEOUTS],
            (unsigned)snapshot.active_cov_subscriptions)) {
        goto encoding_failed;
    }

    for (size_t index = 0; index < SWITCH_INPUT_COUNT; ++index) {
        switch_input_diagnostics_t input;
        if (!switch_input_diagnostics_get(index, &input) ||
            !response_append(
                response,
                OTA_STATUS_RESPONSE_BYTES,
                &response_length,
                "%s{\"gpio\":%d,\"active_low\":%s,\"debounce_ms\":%u,"
                "\"transition_count\":%u,\"last_transition_uptime_ms\":%u,"
                "\"fault\":%s,\"self_test\":{\"run\":%s,"
                "\"passed\":%s,\"pull_down_level\":%s,"
                "\"pull_up_level\":%s},\"startup\":{"
                "\"raw_after_input_enable\":%s,\"config\":",
                index == 0 ? "" : ",",
                input.gpio,
                json_bool(input.active_low),
                (unsigned)input.debounce_ms,
                (unsigned)input.transition_count,
                (unsigned)input.last_transition_uptime_ms,
                json_bool(switch_input_faulted(index)),
                json_bool(input.self_test_run),
                json_bool(input.self_test_passed),
                json_bool(input.self_test_pull_down_level),
                json_bool(input.self_test_pull_up_level),
                json_bool(input.startup_raw_after_input_enable)) ||
            !append_pad_config(
                response,
                OTA_STATUS_RESPONSE_BYTES,
                &response_length,
                &input.startup_config) ||
            !response_append(
                response,
                OTA_STATUS_RESPONSE_BYTES,
                &response_length,
                "},\"configured\":{\"raw\":%s,\"config\":",
                json_bool(input.configured_raw)) ||
            !append_pad_config(
                response,
                OTA_STATUS_RESPONSE_BYTES,
                &response_length,
                &input.configured_config) ||
            !response_append(
                response,
                OTA_STATUS_RESPONSE_BYTES,
                &response_length,
                "},\"current\":{\"raw\":%s,\"stable\":%s,\"config\":",
                json_bool(input.current_raw),
                json_bool(input.stable)) ||
            !append_pad_config(
                response,
                OTA_STATUS_RESPONSE_BYTES,
                &response_length,
                &input.current_config) ||
            !response_append(
                response,
                OTA_STATUS_RESPONSE_BYTES,
                &response_length,
                "}}")) {
            goto encoding_failed;
        }
    }
    if (!response_append(
            response,
            OTA_STATUS_RESPONSE_BYTES,
            &response_length,
            "],\"fault_log\":[")) {
        goto encoding_failed;
    }
    for (size_t index = 0; index < snapshot.fault_log_count; ++index) {
        const diagnostics_fault_event_t *event = &snapshot.fault_log[index];
        if (!response_append(
                response,
                OTA_STATUS_RESPONSE_BYTES,
                &response_length,
                "%s{\"sequence\":%u,\"boot_count\":%u,"
                "\"uptime_ms\":%u,\"type\":\"%s\",\"code\":%d}",
                index == 0 ? "" : ",",
                (unsigned)event->sequence,
                (unsigned)event->boot_count,
                (unsigned)event->uptime_ms,
                diagnostics_event_name(
                    (diagnostics_event_type_t)event->type),
                (int)event->code)) {
            goto encoding_failed;
        }
    }
    if (!response_append(
            response,
            OTA_STATUS_RESPONSE_BYTES,
            &response_length,
            "]}")) {
        goto encoding_failed;
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const esp_err_t send_result =
        httpd_resp_send(request, response, response_length);
    free(response);
    return send_result;

encoding_failed:
    free(response);
    return httpd_resp_send_err(
        request, HTTPD_500_INTERNAL_SERVER_ERROR, "status encoding failed");
}

static esp_err_t input_self_test_post_handler(httpd_req_t *request)
{
    if (!request_authenticated(request)) {
        return send_unauthorized(request);
    }
    const esp_err_t result = switch_inputs_run_self_test();
    if (result != ESP_OK) {
        httpd_resp_set_status(request, "409 Conflict");
        return httpd_resp_sendstr(request, "input self-test already running");
    }

    char response[384];
    size_t length = 0U;
    unsigned failures = 0U;
    if (!response_append(
            response,
            sizeof(response),
            &length,
            "{\"warning\":\"disconnect field wiring before running\","
            "\"inputs\":[")) {
        return ESP_FAIL;
    }
    for (size_t index = 0; index < SWITCH_INPUT_COUNT; ++index) {
        switch_input_diagnostics_t input;
        if (!switch_input_diagnostics_get(index, &input)) {
            return ESP_FAIL;
        }
        if (!input.self_test_passed) {
            failures++;
            diagnostics_record_event(
                DIAGNOSTICS_EVENT_INPUT_SELF_TEST_FAILED, input.gpio);
        }
        if (!response_append(
                response,
                sizeof(response),
                &length,
                "%s{\"gpio\":%d,\"passed\":%s,"
                "\"pull_down_level\":%s,\"pull_up_level\":%s}",
                index == 0 ? "" : ",",
                input.gpio,
                json_bool(input.self_test_passed),
                json_bool(input.self_test_pull_down_level),
                json_bool(input.self_test_pull_up_level))) {
            return ESP_FAIL;
        }
    }
    if (!response_append(
            response,
            sizeof(response),
            &length,
            "],\"failure_count\":%u}",
            failures)) {
        return ESP_FAIL;
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, response, length);
}

static esp_err_t ota_post_handler(httpd_req_t *request)
{
    if (!request_authenticated(request)) {
        return send_unauthorized(request);
    }
    esp_err_t rejection_result = ESP_OK;
    if (!running_image_allows_update(request, &rejection_result)) {
        return rejection_result;
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

    uint8_t candidate_digest[OTA_SHA256_BYTES];
    char candidate_sha256[OTA_SHA256_HEX_BYTES + 1U];
    result = esp_partition_get_sha256(update_partition, candidate_digest);
    if (result != ESP_OK) {
        error_message = "firmware image hash validation failed";
        goto fail;
    }
    sha256_to_hex(candidate_digest, candidate_sha256);

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

    diagnostics_record_ota_result(DIAGNOSTICS_OTA_ACCEPTED, 0);

    char response[256];
    const int response_length = snprintf(
        response,
        sizeof(response),
        "{\"accepted\":true,\"version\":\"%.31s\","
        "\"partition\":\"%.15s\",\"image_sha256\":\"%s\","
        "\"rebooting\":true}",
        candidate.version,
        update_partition->label,
        candidate_sha256);
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
    diagnostics_record_ota_result(DIAGNOSTICS_OTA_FAILED, result);
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
    atomic_store_explicit(&server_ready, false, memory_order_release);
    ESP_RETURN_ON_FALSE(
        load_embedded_token(),
        ESP_ERR_INVALID_ARG,
        TAG,
        "embedded OTA token must contain 32-128 printable characters");
    ESP_RETURN_ON_ERROR(
        validate_embedded_tls_credentials(),
        TAG,
        "embedded HTTPS credentials are unusable");
    ESP_RETURN_ON_ERROR(
        cache_running_image_sha256(),
        TAG,
        "running firmware failed integrity preflight");

    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    config.port_secure = CONFIG_OTA_HTTPS_PORT;
    config.httpd.max_uri_handlers = 3;
    config.httpd.max_open_sockets = 2;
    config.httpd.lru_purge_enable = true;
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
    const httpd_uri_t input_self_test_uri = {
        .uri = "/diagnostics/input-self-test",
        .method = HTTP_POST,
        .handler = input_self_test_post_handler,
    };
    esp_err_t result =
        httpd_register_uri_handler(created_server, &status_uri);
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(created_server, &update_uri);
    }
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(
            created_server, &input_self_test_uri);
    }
    if (result != ESP_OK) {
        (void)httpd_ssl_stop(created_server);
        return result;
    }

    atomic_store_explicit(
        &upload_in_progress, false, memory_order_release);
    server_handle = created_server;
    atomic_store_explicit(&server_ready, true, memory_order_release);
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

bool ota_server_ready(void)
{
#if CONFIG_OTA_HTTPS_ENABLED
    return atomic_load_explicit(&server_ready, memory_order_acquire);
#else
    return false;
#endif
}

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
static bool rollback_runtime_healthy(void)
{
    diagnostics_snapshot_t snapshot;
    bool ota_healthy = true;
#if CONFIG_OTA_HTTPS_ENABLED
    ota_healthy = ota_server_ready();
#endif
    return ota_healthy && bacnet_server_ready() &&
        diagnostics_snapshot_get(&snapshot) && snapshot.network.link_up &&
        snapshot.network.ipv4_address != 0U &&
        snapshot.task_watchdog_subscribed[DIAGNOSTICS_TASK_SWITCH_INPUTS] &&
        snapshot.task_healthy[DIAGNOSTICS_TASK_SWITCH_INPUTS] &&
        snapshot.task_watchdog_subscribed[DIAGNOSTICS_TASK_BACNET] &&
        snapshot.task_healthy[DIAGNOSTICS_TASK_BACNET];
}

static void rollback_validation_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(OTA_ROLLBACK_VALIDATION_INITIAL_DELAY_MS));

    ota_health_gate_t health_gate = {0};
    uint32_t elapsed_ms = OTA_ROLLBACK_VALIDATION_INITIAL_DELAY_MS;
    while (elapsed_ms < OTA_ROLLBACK_VALIDATION_TIMEOUT_MS) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
        if (running == NULL) {
            diagnostics_record_ota_result(
                DIAGNOSTICS_OTA_FAILED, ESP_ERR_NOT_FOUND);
            ESP_LOGE(TAG, "running partition disappeared during validation");
            esp_restart();
        }
        const esp_err_t state_result =
            esp_ota_get_state_partition(running, &state);
        if (state_result != ESP_OK) {
            diagnostics_record_ota_result(
                DIAGNOSTICS_OTA_FAILED, state_result);
            ESP_LOGE(
                TAG,
                "could not read OTA state during validation: %s",
                esp_err_to_name(state_result));
            esp_restart();
        }
        if (state != ESP_OTA_IMG_PENDING_VERIFY) {
            vTaskDelete(NULL);
            return;
        }
        if (ota_health_gate_sample(
                &health_gate,
                rollback_runtime_healthy(),
                OTA_ROLLBACK_HEALTHY_SAMPLES)) {
            const esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
            if (result == ESP_OK) {
                diagnostics_record_ota_result(
                    DIAGNOSTICS_OTA_VALIDATED, 0);
                ESP_LOGI(
                    TAG,
                    "firmware marked valid after %u consecutive healthy samples",
                    OTA_ROLLBACK_HEALTHY_SAMPLES);
            } else {
                diagnostics_record_ota_result(
                    DIAGNOSTICS_OTA_FAILED, result);
                ESP_LOGE(
                    TAG,
                    "failed to confirm running firmware: %s",
                    esp_err_to_name(result));
                esp_restart();
            }
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(OTA_ROLLBACK_VALIDATION_POLL_MS));
        elapsed_ms += OTA_ROLLBACK_VALIDATION_POLL_MS;
    }

    diagnostics_record_ota_result(DIAGNOSTICS_OTA_FAILED, ESP_ERR_TIMEOUT);
    ESP_LOGE(TAG, "runtime health check failed; rolling back firmware");
    const esp_err_t result = esp_ota_mark_app_invalid_rollback_and_reboot();
    ESP_LOGE(TAG, "firmware rollback failed: %s", esp_err_to_name(result));
    esp_restart();
}
#endif

esp_err_t ota_start_rollback_validation(void)
{
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_RETURN_ON_FALSE(
        running != NULL,
        ESP_ERR_NOT_FOUND,
        TAG,
        "running partition not found for OTA validation");
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    const esp_err_t state_result =
        esp_ota_get_state_partition(running, &state);
    if (state_result == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(
        state_result, TAG, "could not read running OTA image state");
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
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
        "new firmware pending; health validation begins in %u ms",
        OTA_ROLLBACK_VALIDATION_INITIAL_DELAY_MS);
#endif
    return ESP_OK;
}
