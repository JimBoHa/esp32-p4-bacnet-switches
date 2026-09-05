#include "ota_server.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "bacnet_server.h"
#include "cJSON.h"
#include "config_store.h"
#include "diagnostics.h"
#include "diagnostics_time.h"
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
#include "network_config_store.h"
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
#define CONFIG_JSON_MAX_BYTES 4096U

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

static bool json_required_unsigned(
    const cJSON *object,
    const char *name,
    uint32_t maximum,
    uint32_t *value,
    char *reason,
    size_t reason_capacity)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0.0 ||
        item->valuedouble > (double)maximum ||
        (double)(uint32_t)item->valuedouble != item->valuedouble) {
        (void)snprintf(
            reason, reason_capacity, "%s must be an integer 0..%u", name,
            (unsigned)maximum);
        return false;
    }
    *value = (uint32_t)item->valuedouble;
    return true;
}

static bool json_required_string(
    const cJSON *object,
    const char *name,
    char *value,
    size_t value_capacity,
    char *reason,
    size_t reason_capacity)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        strlen(item->valuestring) >= value_capacity) {
        (void)snprintf(
            reason,
            reason_capacity,
            "%s must be a string shorter than %u bytes",
            name,
            (unsigned)value_capacity);
        return false;
    }
    memset(value, 0, value_capacity);
    memcpy(value, item->valuestring, strlen(item->valuestring));
    return true;
}

static bool parse_config_json(
    const cJSON *root,
    firmware_config_t *config,
    char *reason,
    size_t reason_capacity)
{
    uint32_t value = 0U;
    if (!cJSON_IsObject(root) || config == NULL) {
        (void)snprintf(reason, reason_capacity, "configuration must be an object");
        return false;
    }
    memset(config, 0, sizeof(*config));
    if (!json_required_unsigned(
            root,
            "schema",
            FIRMWARE_CONFIG_SCHEMA,
            &value,
            reason,
            reason_capacity) ||
        value != FIRMWARE_CONFIG_SCHEMA) {
        (void)snprintf(
            reason,
            reason_capacity,
            "schema must be %u",
            (unsigned)FIRMWARE_CONFIG_SCHEMA);
        return false;
    }
    if (!json_required_unsigned(
            root,
            "device_instance",
            4194302U,
            &config->device_instance,
            reason,
            reason_capacity) ||
        !json_required_unsigned(
            root, "bacnet_port", UINT16_MAX, &value, reason, reason_capacity)) {
        return false;
    }
    config->bacnet_port = (uint16_t)value;
    if (config->bacnet_port == 0U) {
        (void)snprintf(reason, reason_capacity, "bacnet_port must be 1..65535");
        return false;
    }
    if (!json_required_unsigned(
            root,
            "vendor_identifier",
            UINT16_MAX,
            &value,
            reason,
            reason_capacity)) {
        return false;
    }
    config->vendor_identifier = (uint16_t)value;
    if (!json_required_unsigned(
            root, "debounce_ms", 500U, &value, reason, reason_capacity)) {
        return false;
    }
    config->debounce_ms = (uint16_t)value;
    if (!json_required_string(
            root,
            "device_name",
            config->device_name,
            sizeof(config->device_name),
            reason,
            reason_capacity) ||
        !json_required_string(
            root,
            "vendor_name",
            config->vendor_name,
            sizeof(config->vendor_name),
            reason,
            reason_capacity) ||
        !json_required_string(
            root,
            "location",
            config->location,
            sizeof(config->location),
            reason,
            reason_capacity)) {
        return false;
    }

    const cJSON *inputs = cJSON_GetObjectItemCaseSensitive(root, "inputs");
    if (!cJSON_IsArray(inputs) ||
        cJSON_GetArraySize(inputs) != FIRMWARE_CONFIG_INPUT_COUNT) {
        (void)snprintf(
            reason,
            reason_capacity,
            "inputs must contain exactly %u entries",
            (unsigned)FIRMWARE_CONFIG_INPUT_COUNT);
        return false;
    }
    static const uint32_t INPUT_GPIOS[FIRMWARE_CONFIG_INPUT_COUNT] = {
        CONFIG_TOGGLE_INPUT_1_GPIO,
        CONFIG_TOGGLE_INPUT_2_GPIO,
        CONFIG_TOGGLE_INPUT_3_GPIO,
    };
    for (size_t index = 0U;
         index < FIRMWARE_CONFIG_INPUT_COUNT;
         ++index) {
        const cJSON *input = cJSON_GetArrayItem(inputs, (int)index);
        uint32_t gpio = 0U;
        if (!cJSON_IsObject(input) ||
            !json_required_unsigned(
                input, "gpio", 54U, &gpio, reason, reason_capacity) ||
            gpio != INPUT_GPIOS[index] ||
            !json_required_unsigned(
                input,
                "object_instance",
                4194302U,
                &config->input_instances[index],
                reason,
                reason_capacity) ||
            !json_required_string(
                input,
                "name",
                config->input_names[index],
                sizeof(config->input_names[index]),
                reason,
                reason_capacity)) {
            if (gpio != INPUT_GPIOS[index]) {
                (void)snprintf(
                    reason,
                    reason_capacity,
                    "input %u gpio is fixed at %u",
                    (unsigned)(index + 1U),
                    (unsigned)INPUT_GPIOS[index]);
            }
            return false;
        }
        const cJSON *active_low =
            cJSON_GetObjectItemCaseSensitive(input, "active_low");
        if (!cJSON_IsBool(active_low)) {
            (void)snprintf(
                reason,
                reason_capacity,
                "input %u active_low must be boolean",
                (unsigned)(index + 1U));
            return false;
        }
        if (cJSON_IsTrue(active_low)) {
            config->input_active_low_mask |= (uint8_t)(1U << index);
        }
    }
    return config_model_validate(config, reason, reason_capacity);
}

static cJSON *config_json(
    const firmware_config_t *saved,
    const firmware_config_t *active,
    bool include_result,
    bool changed)
{
    static const int INPUT_GPIOS[FIRMWARE_CONFIG_INPUT_COUNT] = {
        CONFIG_TOGGLE_INPUT_1_GPIO,
        CONFIG_TOGGLE_INPUT_2_GPIO,
        CONFIG_TOGGLE_INPUT_3_GPIO,
    };
    cJSON *root = cJSON_CreateObject();
    cJSON *inputs = cJSON_CreateArray();
    if (root == NULL || inputs == NULL ||
        cJSON_AddNumberToObject(root, "schema", FIRMWARE_CONFIG_SCHEMA) == NULL ||
        cJSON_AddNumberToObject(
            root, "database_revision", saved->database_revision) == NULL ||
        cJSON_AddNumberToObject(
            root,
            "active_database_revision",
            active->database_revision) == NULL ||
        cJSON_AddBoolToObject(
            root, "restart_required", config_store_restart_required()) == NULL ||
        cJSON_AddNumberToObject(
            root, "device_instance", saved->device_instance) == NULL ||
        cJSON_AddStringToObject(root, "device_name", saved->device_name) == NULL ||
        cJSON_AddNumberToObject(root, "bacnet_port", saved->bacnet_port) == NULL ||
        cJSON_AddNumberToObject(
            root, "vendor_identifier", saved->vendor_identifier) == NULL ||
        cJSON_AddStringToObject(root, "vendor_name", saved->vendor_name) == NULL ||
        cJSON_AddStringToObject(root, "location", saved->location) == NULL ||
        cJSON_AddNumberToObject(root, "debounce_ms", saved->debounce_ms) == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(inputs);
        return NULL;
    }
    if (include_result &&
        (cJSON_AddBoolToObject(root, "accepted", true) == NULL ||
         cJSON_AddBoolToObject(root, "changed", changed) == NULL)) {
        cJSON_Delete(root);
        cJSON_Delete(inputs);
        return NULL;
    }
    for (size_t index = 0U;
         index < FIRMWARE_CONFIG_INPUT_COUNT;
         ++index) {
        cJSON *input = cJSON_CreateObject();
        if (input == NULL ||
            cJSON_AddNumberToObject(input, "gpio", INPUT_GPIOS[index]) == NULL ||
            cJSON_AddNumberToObject(
                input,
                "object_instance",
                saved->input_instances[index]) == NULL ||
            cJSON_AddStringToObject(
                input, "name", saved->input_names[index]) == NULL ||
            cJSON_AddBoolToObject(
                input,
                "active_low",
                (saved->input_active_low_mask & (1U << index)) != 0U) == NULL ||
            !cJSON_AddItemToArray(inputs, input)) {
            cJSON_Delete(input);
            cJSON_Delete(inputs);
            cJSON_Delete(root);
            return NULL;
        }
    }
    if (!cJSON_AddItemToObject(root, "inputs", inputs)) {
        cJSON_Delete(inputs);
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static esp_err_t send_config_json(
    httpd_req_t *request,
    bool include_result,
    bool changed)
{
    firmware_config_t saved;
    firmware_config_t active;
    config_store_get_saved(&saved);
    config_store_get_active(&active);
    cJSON *root = config_json(&saved, &active, include_result, changed);
    if (root == NULL) {
        return httpd_resp_send_err(
            request, HTTPD_500_INTERNAL_SERVER_ERROR, "configuration encoding failed");
    }
    char *encoded = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (encoded == NULL) {
        return httpd_resp_send_err(
            request, HTTPD_500_INTERNAL_SERVER_ERROR, "configuration encoding failed");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const esp_err_t result = httpd_resp_sendstr(request, encoded);
    cJSON_free(encoded);
    return result;
}

static esp_err_t config_get_handler(httpd_req_t *request)
{
    if (!request_authenticated(request)) {
        return send_unauthorized(request);
    }
    return send_config_json(request, false, false);
}

static esp_err_t config_put_handler(httpd_req_t *request)
{
    if (!request_authenticated(request)) {
        return send_unauthorized(request);
    }
    char content_type[64];
    const size_t content_type_length =
        httpd_req_get_hdr_value_len(request, "Content-Type");
    if (content_type_length == 0U ||
        content_type_length >= sizeof(content_type) ||
        httpd_req_get_hdr_value_str(
            request,
            "Content-Type",
            content_type,
            sizeof(content_type)) != ESP_OK ||
        strncasecmp(content_type, "application/json", 16U) != 0 ||
        (content_type[16] != '\0' && content_type[16] != ';' &&
         content_type[16] != ' ' && content_type[16] != '\t')) {
        httpd_resp_set_status(request, "415 Unsupported Media Type");
        return httpd_resp_sendstr(
            request, "Content-Type must be application/json");
    }
    if (request->content_len == 0U ||
        request->content_len > CONFIG_JSON_MAX_BYTES) {
        return httpd_resp_send_err(
            request, HTTPD_400_BAD_REQUEST, "configuration body must be 1..4096 bytes");
    }
    char *body = malloc(request->content_len + 1U);
    if (body == NULL) {
        return httpd_resp_send_err(
            request, HTTPD_500_INTERNAL_SERVER_ERROR, "not enough memory");
    }
    size_t received_total = 0U;
    unsigned timeouts = 0U;
    while (received_total < request->content_len) {
        const int received = httpd_req_recv(
            request,
            body + received_total,
            request->content_len - received_total);
        if (received == HTTPD_SOCK_ERR_TIMEOUT && timeouts++ < 5U) {
            continue;
        }
        if (received <= 0) {
            free(body);
            return httpd_resp_send_err(
                request, HTTPD_400_BAD_REQUEST, "incomplete configuration body");
        }
        received_total += (size_t)received;
    }
    body[received_total] = '\0';

    cJSON *root = cJSON_ParseWithLengthOpts(
        body, received_total + 1U, NULL, true);
    free(body);
    char reason[160] = "invalid JSON";
    firmware_config_t candidate;
    if (root == NULL || !parse_config_json(
            root, &candidate, reason, sizeof(reason))) {
        cJSON_Delete(root);
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, reason);
    }
    cJSON_Delete(root);

    bool changed = false;
    const esp_err_t result = config_store_update(&candidate, &changed);
    if (result != ESP_OK) {
        return httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "failed to persist configuration");
    }
    httpd_resp_set_status(request, "202 Accepted");
    return send_config_json(request, true, changed);
}

static bool parse_ipv4_text(
    const char *text,
    uint8_t address[4])
{
    unsigned octets[4];
    char trailing;
    if (text == NULL || sscanf(
            text,
            "%u.%u.%u.%u%c",
            &octets[0],
            &octets[1],
            &octets[2],
            &octets[3],
            &trailing) != 4) {
        return false;
    }
    for (size_t index = 0U; index < 4U; ++index) {
        if (octets[index] > UINT8_MAX) {
            return false;
        }
        address[index] = (uint8_t)octets[index];
    }
    return true;
}

static bool json_required_ipv4(
    const cJSON *object,
    const char *name,
    uint8_t address[4],
    char *reason,
    size_t reason_capacity)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) ||
        !parse_ipv4_text(item->valuestring, address)) {
        (void)snprintf(
            reason, reason_capacity, "%s must be an IPv4 address", name);
        return false;
    }
    return true;
}

static bool parse_network_config_json(
    const cJSON *root,
    network_config_t *config,
    char *reason,
    size_t reason_capacity)
{
    if (!cJSON_IsObject(root) || config == NULL) {
        (void)snprintf(
            reason, reason_capacity, "network configuration must be an object");
        return false;
    }
    memset(config, 0, sizeof(*config));
    uint32_t schema = 0U;
    if (!json_required_unsigned(
            root,
            "schema",
            NETWORK_CONFIG_SCHEMA,
            &schema,
            reason,
            reason_capacity) ||
        schema != NETWORK_CONFIG_SCHEMA) {
        (void)snprintf(
            reason,
            reason_capacity,
            "schema must be %u",
            (unsigned)NETWORK_CONFIG_SCHEMA);
        return false;
    }
    if (!json_required_string(
            root,
            "hostname",
            config->hostname,
            sizeof(config->hostname),
            reason,
            reason_capacity)) {
        return false;
    }
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    const cJSON *static_addresses =
        cJSON_GetObjectItemCaseSensitive(root, "static");
    if (!cJSON_IsString(mode) || mode->valuestring == NULL) {
        (void)snprintf(reason, reason_capacity, "mode must be dhcp or static");
        return false;
    }
    if (strcmp(mode->valuestring, "dhcp") == 0) {
        config->mode = NETWORK_ADDRESS_DHCP;
        if (!cJSON_IsNull(static_addresses)) {
            (void)snprintf(
                reason, reason_capacity, "static must be null in DHCP mode");
            return false;
        }
    } else if (strcmp(mode->valuestring, "static") == 0) {
        config->mode = NETWORK_ADDRESS_STATIC;
        if (!cJSON_IsObject(static_addresses) ||
            !json_required_ipv4(
                static_addresses,
                "ipv4",
                config->ipv4,
                reason,
                reason_capacity) ||
            !json_required_ipv4(
                static_addresses,
                "netmask",
                config->netmask,
                reason,
                reason_capacity) ||
            !json_required_ipv4(
                static_addresses,
                "gateway",
                config->gateway,
                reason,
                reason_capacity) ||
            !json_required_ipv4(
                static_addresses,
                "dns",
                config->dns,
                reason,
                reason_capacity)) {
            if (!cJSON_IsObject(static_addresses)) {
                (void)snprintf(
                    reason,
                    reason_capacity,
                    "static must contain IPv4 settings in static mode");
            }
            return false;
        }
    } else {
        (void)snprintf(reason, reason_capacity, "mode must be dhcp or static");
        return false;
    }
    return network_config_validate(config, reason, reason_capacity);
}

static cJSON *network_config_json(
    const network_config_t *saved,
    const network_config_t *active,
    const network_config_t *confirmed,
    bool include_result,
    bool changed)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL ||
        cJSON_AddNumberToObject(root, "schema", NETWORK_CONFIG_SCHEMA) == NULL ||
        cJSON_AddNumberToObject(root, "revision", saved->revision) == NULL ||
        cJSON_AddNumberToObject(
            root, "active_revision", active->revision) == NULL ||
        cJSON_AddNumberToObject(
            root, "confirmed_revision", confirmed->revision) == NULL ||
        cJSON_AddBoolToObject(
            root,
            "restart_required",
            network_config_restart_required()) == NULL ||
        cJSON_AddBoolToObject(
            root, "trial_active", network_config_trial_active()) == NULL ||
        cJSON_AddNumberToObject(
            root,
            "trial_seconds_remaining",
            network_config_trial_seconds_remaining()) == NULL ||
        cJSON_AddNumberToObject(
            root,
            "trial_timeout_seconds",
            NETWORK_CONFIG_TRIAL_TIMEOUT_SECONDS) == NULL ||
        cJSON_AddStringToObject(
            root,
            "mode",
            saved->mode == NETWORK_ADDRESS_DHCP ? "dhcp" : "static") == NULL ||
        cJSON_AddStringToObject(root, "hostname", saved->hostname) == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    if (include_result &&
        (cJSON_AddBoolToObject(root, "accepted", true) == NULL ||
         cJSON_AddBoolToObject(root, "changed", changed) == NULL)) {
        cJSON_Delete(root);
        return NULL;
    }
    if (saved->mode == NETWORK_ADDRESS_DHCP) {
        if (cJSON_AddNullToObject(root, "static") == NULL) {
            cJSON_Delete(root);
            return NULL;
        }
    } else {
        char ipv4[16];
        char netmask[16];
        char gateway[16];
        char dns[16];
        (void)snprintf(
            ipv4,
            sizeof(ipv4),
            "%u.%u.%u.%u",
            saved->ipv4[0],
            saved->ipv4[1],
            saved->ipv4[2],
            saved->ipv4[3]);
        (void)snprintf(
            netmask,
            sizeof(netmask),
            "%u.%u.%u.%u",
            saved->netmask[0],
            saved->netmask[1],
            saved->netmask[2],
            saved->netmask[3]);
        (void)snprintf(
            gateway,
            sizeof(gateway),
            "%u.%u.%u.%u",
            saved->gateway[0],
            saved->gateway[1],
            saved->gateway[2],
            saved->gateway[3]);
        (void)snprintf(
            dns,
            sizeof(dns),
            "%u.%u.%u.%u",
            saved->dns[0],
            saved->dns[1],
            saved->dns[2],
            saved->dns[3]);
        cJSON *addresses = cJSON_CreateObject();
        if (addresses == NULL ||
            cJSON_AddStringToObject(addresses, "ipv4", ipv4) == NULL ||
            cJSON_AddStringToObject(addresses, "netmask", netmask) == NULL ||
            cJSON_AddStringToObject(addresses, "gateway", gateway) == NULL ||
            cJSON_AddStringToObject(addresses, "dns", dns) == NULL ||
            !cJSON_AddItemToObject(root, "static", addresses)) {
            cJSON_Delete(addresses);
            cJSON_Delete(root);
            return NULL;
        }
    }
    return root;
}

static esp_err_t send_network_config_json(
    httpd_req_t *request,
    bool include_result,
    bool changed)
{
    network_config_t saved;
    network_config_t active;
    network_config_t confirmed;
    network_config_get_saved(&saved);
    network_config_get_active(&active);
    network_config_get_confirmed(&confirmed);
    cJSON *root = network_config_json(
        &saved, &active, &confirmed, include_result, changed);
    if (root == NULL) {
        return httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "network configuration encoding failed");
    }
    char *encoded = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (encoded == NULL) {
        return httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "network configuration encoding failed");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const esp_err_t result = httpd_resp_sendstr(request, encoded);
    cJSON_free(encoded);
    return result;
}

static esp_err_t network_config_get_handler(httpd_req_t *request)
{
    if (!request_authenticated(request)) {
        return send_unauthorized(request);
    }
    return send_network_config_json(request, false, false);
}

static esp_err_t network_config_put_handler(httpd_req_t *request)
{
    if (!request_authenticated(request)) {
        return send_unauthorized(request);
    }
    char content_type[64];
    const size_t content_type_length =
        httpd_req_get_hdr_value_len(request, "Content-Type");
    if (content_type_length == 0U ||
        content_type_length >= sizeof(content_type) ||
        httpd_req_get_hdr_value_str(
            request,
            "Content-Type",
            content_type,
            sizeof(content_type)) != ESP_OK ||
        strncasecmp(content_type, "application/json", 16U) != 0 ||
        (content_type[16] != '\0' && content_type[16] != ';' &&
         content_type[16] != ' ' && content_type[16] != '\t')) {
        httpd_resp_set_status(request, "415 Unsupported Media Type");
        return httpd_resp_sendstr(
            request, "Content-Type must be application/json");
    }
    if (request->content_len == 0U ||
        request->content_len > CONFIG_JSON_MAX_BYTES) {
        return httpd_resp_send_err(
            request,
            HTTPD_400_BAD_REQUEST,
            "network configuration body must be 1..4096 bytes");
    }
    char *body = malloc(request->content_len + 1U);
    if (body == NULL) {
        return httpd_resp_send_err(
            request, HTTPD_500_INTERNAL_SERVER_ERROR, "not enough memory");
    }
    size_t received_total = 0U;
    unsigned timeouts = 0U;
    while (received_total < request->content_len) {
        const int received = httpd_req_recv(
            request,
            body + received_total,
            request->content_len - received_total);
        if (received == HTTPD_SOCK_ERR_TIMEOUT && timeouts++ < 5U) {
            continue;
        }
        if (received <= 0) {
            free(body);
            return httpd_resp_send_err(
                request,
                HTTPD_400_BAD_REQUEST,
                "incomplete network configuration body");
        }
        received_total += (size_t)received;
    }
    body[received_total] = '\0';
    cJSON *root = cJSON_ParseWithLengthOpts(
        body, received_total + 1U, NULL, true);
    free(body);
    char reason[160] = "invalid JSON";
    network_config_t candidate;
    if (root == NULL || !parse_network_config_json(
            root, &candidate, reason, sizeof(reason))) {
        cJSON_Delete(root);
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, reason);
    }
    cJSON_Delete(root);

    bool changed = false;
    const esp_err_t result = network_config_update(&candidate, &changed);
    if (result == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(request, "409 Conflict");
        return httpd_resp_sendstr(
            request, "confirm the active network trial before making changes");
    }
    if (result != ESP_OK) {
        return httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "failed to persist network configuration");
    }
    httpd_resp_set_status(request, "202 Accepted");
    return send_network_config_json(request, true, changed);
}

static esp_err_t network_config_confirm_handler(httpd_req_t *request)
{
    if (!request_authenticated(request)) {
        return send_unauthorized(request);
    }
    const esp_err_t result = network_config_confirm_trial();
    if (result == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(request, "409 Conflict");
        return httpd_resp_sendstr(request, "no active network trial");
    }
    if (result != ESP_OK) {
        return httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "failed to confirm network configuration");
    }
    return send_network_config_json(request, true, false);
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
    firmware_config_t active_config;
    firmware_config_t saved_config;
    network_config_t active_network_config;
    network_config_t saved_network_config;
    network_config_t confirmed_network_config;
    config_store_get_active(&active_config);
    config_store_get_saved(&saved_config);
    network_config_get_active(&active_network_config);
    network_config_get_saved(&saved_network_config);
    network_config_get_confirmed(&confirmed_network_config);

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
            "\"system\":{\"uptime_ms\":%" PRIu64
            ",\"chip_temperature_c\":",
            app != NULL ? app->project_name : "unknown",
            app != NULL ? app->version : "unknown",
            esp_get_idf_version(),
            running != NULL ? running->label : "unknown",
            ota_state_name(image_state),
            (unsigned)CONFIG_OTA_HTTPS_PORT,
            diagnostics_git_revision(),
            running_image_sha256,
            snapshot.uptime_ms)) {
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
            "\"last_heartbeat_ms\":%" PRIu64 "},"
            "\"bacnet\":{\"subscribed\":%s,\"healthy\":%s,"
            "\"last_heartbeat_ms\":%" PRIu64 "}}},",
            (unsigned)snapshot.free_heap_bytes,
            (unsigned)snapshot.minimum_free_heap_bytes,
            (unsigned)snapshot.reset_reason,
            diagnostics_reset_reason_name(snapshot.reset_reason),
            (unsigned)snapshot.boot_count,
            diagnostics_ota_result_name(snapshot.last_ota_result),
            json_bool(snapshot.task_watchdog_subscribed[
                DIAGNOSTICS_TASK_SWITCH_INPUTS]),
            json_bool(snapshot.task_healthy[DIAGNOSTICS_TASK_SWITCH_INPUTS]),
            snapshot.task_last_heartbeat_ms[DIAGNOSTICS_TASK_SWITCH_INPUTS],
            json_bool(snapshot.task_watchdog_subscribed[
                DIAGNOSTICS_TASK_BACNET]),
            json_bool(snapshot.task_healthy[DIAGNOSTICS_TASK_BACNET]),
            snapshot.task_last_heartbeat_ms[DIAGNOSTICS_TASK_BACNET])) {
        goto encoding_failed;
    }
    if (!response_append(
            response,
            OTA_STATUS_RESPONSE_BYTES,
            &response_length,
            "\"configuration\":{\"active_database_revision\":%u,"
            "\"saved_database_revision\":%u,\"restart_required\":%s},",
            (unsigned)active_config.database_revision,
            (unsigned)saved_config.database_revision,
            json_bool(config_store_restart_required()))) {
        goto encoding_failed;
    }
    if (!response_append(
            response,
            OTA_STATUS_RESPONSE_BYTES,
            &response_length,
            "\"network_configuration\":{\"mode\":\"%s\","
            "\"hostname\":\"%s\",\"active_revision\":%u,"
            "\"saved_revision\":%u,\"confirmed_revision\":%u,"
            "\"restart_required\":%s,\"trial_active\":%s,"
            "\"trial_seconds_remaining\":%u},",
            active_network_config.mode == NETWORK_ADDRESS_DHCP
                ? "dhcp"
                : "static",
            active_network_config.hostname,
            (unsigned)active_network_config.revision,
            (unsigned)saved_network_config.revision,
            (unsigned)confirmed_network_config.revision,
            json_bool(network_config_restart_required()),
            json_bool(network_config_trial_active()),
            (unsigned)network_config_trial_seconds_remaining())) {
        goto encoding_failed;
    }

    const esp_ip4_addr_t ipv4 = {.addr = snapshot.network.ipv4_address};
    const esp_ip4_addr_t netmask = {.addr = snapshot.network.ipv4_netmask};
    const esp_ip4_addr_t gateway = {.addr = snapshot.network.ipv4_gateway};
    const uint64_t address_age_ms = snapshot.network.ipv4_address != 0U
        ? diagnostics_elapsed_milliseconds(
              snapshot.uptime_ms, snapshot.network.ip_acquired_uptime_ms)
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
            "\"dhcp_status\":\"%s\",\"ip_acquired_uptime_ms\":%" PRIu64
            ",\"address_age_ms\":%" PRIu64 "},",
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
            snapshot.network.ip_acquired_uptime_ms,
            address_age_ms) ||
        !response_append(
            response,
            OTA_STATUS_RESPONSE_BYTES,
            &response_length,
            "\"bacnet\":{\"rx\":%u,\"who_is\":%u,\"who_has\":%u,"
            "\"read_property\":%u,\"read_property_multiple\":%u,"
            "\"subscribe_cov\":%u,\"malformed\":%u,\"ignored\":%u,"
            "\"responses\":%u,\"errors\":%u,\"rate_limited\":%u,"
            "\"cov_sent\":%u,\"cov_acked\":%u,\"cov_timeouts\":%u,"
            "\"active_cov_subscriptions\":%u},\"gpio_diagnostics\":[",
            (unsigned)snapshot.bacnet[DIAGNOSTICS_BACNET_RX],
            (unsigned)snapshot.bacnet[DIAGNOSTICS_BACNET_WHO_IS],
            (unsigned)snapshot.bacnet[DIAGNOSTICS_BACNET_WHO_HAS],
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
                "\"transition_count\":%u,"
                "\"last_transition_uptime_ms\":%" PRIu64 ","
                "\"signal\":{\"raw_edge_count\":%u,"
                "\"accepted_transition_count\":%u,"
                "\"rejected_pulse_count\":%u,"
                "\"chatter_event_count\":%u,\"chattering\":%s,"
                "\"candidate_active\":%s,\"candidate_level\":%s,"
                "\"candidate_age_ms\":%u,"
                "\"last_raw_edge_uptime_ms\":%" PRIu64 ","
                "\"last_rejected_pulse_uptime_ms\":%" PRIu64 ","
                "\"last_rejected_pulse_width_ms\":%u},"
                "\"fault\":%s,\"self_test\":{\"run\":%s,"
                "\"passed\":%s,\"classification\":\"%s\","
                "\"pull_down_level\":%s,\"pull_down_stable\":%s,"
                "\"pull_up_level\":%s,\"pull_up_stable\":%s},"
                "\"startup\":{"
                "\"raw_after_input_enable\":%s,\"config\":",
                index == 0 ? "" : ",",
                input.gpio,
                json_bool(input.active_low),
                (unsigned)input.debounce_ms,
                (unsigned)input.transition_count,
                input.last_transition_uptime_ms,
                (unsigned)input.raw_edge_count,
                (unsigned)input.accepted_transition_count,
                (unsigned)input.rejected_pulse_count,
                (unsigned)input.chatter_event_count,
                json_bool(input.chattering),
                json_bool(input.candidate_active),
                json_bool(input.candidate_level),
                (unsigned)input.candidate_age_ms,
                input.last_raw_edge_uptime_ms,
                input.last_rejected_pulse_uptime_ms,
                (unsigned)input.last_rejected_pulse_width_ms,
                json_bool(switch_input_faulted(index)),
                json_bool(input.self_test_run),
                json_bool(input.self_test_passed),
                input_line_classification_name(
                    input.self_test_classification),
                json_bool(input.self_test_pull_down_level),
                json_bool(input.self_test_pull_down_stable),
                json_bool(input.self_test_pull_up_level),
                json_bool(input.self_test_pull_up_stable),
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
                "\"uptime_ms\":%" PRIu64
                ",\"type\":\"%s\",\"code\":%d}",
                index == 0 ? "" : ",",
                (unsigned)event->sequence,
                (unsigned)event->boot_count,
                event->uptime_ms,
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

    char response[768];
    size_t length = 0U;
    unsigned failures = 0U;
    if (!response_append(
            response,
            sizeof(response),
            &length,
            "{\"notice\":\"test uses internal weak pulls and does not drive outputs\","
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
                "\"classification\":\"%s\","
                "\"pull_down_level\":%s,\"pull_down_stable\":%s,"
                "\"pull_up_level\":%s,\"pull_up_stable\":%s}",
                index == 0 ? "" : ",",
                input.gpio,
                json_bool(input.self_test_passed),
                input_line_classification_name(
                    input.self_test_classification),
                json_bool(input.self_test_pull_down_level),
                json_bool(input.self_test_pull_down_stable),
                json_bool(input.self_test_pull_up_level),
                json_bool(input.self_test_pull_up_stable))) {
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
    config.httpd.max_uri_handlers = 8;
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
    const httpd_uri_t config_get_uri = {
        .uri = "/config",
        .method = HTTP_GET,
        .handler = config_get_handler,
    };
    const httpd_uri_t config_put_uri = {
        .uri = "/config",
        .method = HTTP_PUT,
        .handler = config_put_handler,
    };
    const httpd_uri_t network_config_get_uri = {
        .uri = "/network/config",
        .method = HTTP_GET,
        .handler = network_config_get_handler,
    };
    const httpd_uri_t network_config_put_uri = {
        .uri = "/network/config",
        .method = HTTP_PUT,
        .handler = network_config_put_handler,
    };
    const httpd_uri_t network_config_confirm_uri = {
        .uri = "/network/config/confirm",
        .method = HTTP_POST,
        .handler = network_config_confirm_handler,
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
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(
            created_server, &config_get_uri);
    }
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(
            created_server, &config_put_uri);
    }
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(
            created_server, &network_config_get_uri);
    }
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(
            created_server, &network_config_put_uri);
    }
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(
            created_server, &network_config_confirm_uri);
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
