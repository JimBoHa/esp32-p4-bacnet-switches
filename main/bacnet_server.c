#include "bacnet_server.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bacnet_codec.h"
#include "config_store.h"
#include "cov_retry_cache.h"
#include "diagnostics.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "network_config_store.h"
#include "sdkconfig.h"
#include "switch_inputs.h"

#define BACNET_RESPONSE_CAPACITY (BACNET_MAX_APDU + 6U)
#define BACNET_MAX_RESPONSES_PER_SECOND 50U
#define BACNET_SOCKET_RETRY_MS 1000U
#define BACNET_RECEIVE_TIMEOUT_US 100000U
#define BACNET_MAX_COV_SUBSCRIPTIONS 8U
#define BACNET_COV_RETRY_MS 3000U
#define BACNET_COV_MAX_RETRIES 3U
#define BACNET_COV_MAX_LIFETIME_SECONDS 604800U

enum {
    BACNET_RELIABILITY_NO_FAULT = 0,
    BACNET_RELIABILITY_UNRELIABLE_OTHER = 7,
    BACNET_UNITS_DEGREES_CELSIUS = 62,
    BACNET_UNITS_SECONDS = 73,
    BACNET_UNITS_NO_UNITS = 95,
};

static const char *TAG = "bacnet_ip";
_Static_assert(
    BACNET_PHYSICAL_BINARY_INPUT_COUNT == SWITCH_INPUT_COUNT,
    "BACnet and physical input counts must match");
static const char *const INPUT_DESCRIPTIONS[BACNET_BINARY_INPUT_COUNT] = {
    "Debounced read-only physical toggle input 1",
    "Debounced read-only physical toggle input 2",
    "Debounced read-only physical toggle input 3",
    "Physical Ethernet link is up",
    "Ethernet interface has an IPv4 address",
};
static const uint32_t ANALOG_VALUE_INSTANCES[BACNET_ANALOG_VALUE_COUNT] = {
    1000, 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009, 1010,
};
static const char *const ANALOG_VALUE_NAMES[BACNET_ANALOG_VALUE_COUNT] = {
    "Chip Temperature",
    "System Uptime",
    "Free Heap",
    "Minimum Free Heap",
    "Ethernet Link Losses",
    "Ethernet Reconnects",
    "BACnet RX Packets",
    "BACnet Protocol Errors",
    "Last Reset Reason",
    "Active COV Subscriptions",
    "Boot Count",
};
static const char *const ANALOG_VALUE_DESCRIPTIONS[
    BACNET_ANALOG_VALUE_COUNT] = {
    "ESP32-P4 internal die temperature; not ambient temperature",
    "Seconds elapsed since the current boot",
    "Currently available heap memory in bytes",
    "Lowest available heap memory observed since boot in bytes",
    "Ethernet link-down events observed since boot",
    "Ethernet link reconnections after the initial link-up",
    "BACnet/IP datagrams accepted since boot",
    "Malformed, rejected, aborted, or error BACnet transactions",
    "Numeric ESP-IDF reset reason for the current boot",
    "Currently active BACnet COV subscriptions",
    "Persistent successful-start count including the current boot",
};
static const uint32_t ANALOG_VALUE_UNITS[BACNET_ANALOG_VALUE_COUNT] = {
    BACNET_UNITS_DEGREES_CELSIUS,
    BACNET_UNITS_SECONDS,
    BACNET_UNITS_NO_UNITS,
    BACNET_UNITS_NO_UNITS,
    BACNET_UNITS_NO_UNITS,
    BACNET_UNITS_NO_UNITS,
    BACNET_UNITS_NO_UNITS,
    BACNET_UNITS_NO_UNITS,
    BACNET_UNITS_NO_UNITS,
    BACNET_UNITS_NO_UNITS,
    BACNET_UNITS_NO_UNITS,
};

typedef struct {
    bool active;
    struct sockaddr_in recipient;
    uint32_t process_id;
    size_t input_index;
    bool confirmed;
    int64_t expires_at_us;
    bool dirty;
    bool awaiting_ack;
    uint8_t invoke_id;
    unsigned retry_count;
    int64_t next_retry_us;
    cov_retry_cache_t retry_payload;
} cov_subscription_t;

static TaskHandle_t server_task_handle;
static atomic_bool server_ready;
static portMUX_TYPE server_lock = portMUX_INITIALIZER_UNLOCKED;
static cov_subscription_t cov_subscriptions[BACNET_MAX_COV_SUBSCRIPTIONS];
static uint8_t next_cov_invoke_id = 1U;
static char application_software_version[64];
static firmware_config_t bacnet_config;

static void snapshot_device_state(bacnet_device_state_t *state)
{
    const esp_app_desc_t *app = esp_app_get_description();
    diagnostics_snapshot_t diagnostics;
    network_config_t network_config;
    const bool diagnostics_valid = diagnostics_snapshot_get(&diagnostics);
    network_config_get_active(&network_config);
    (void)snprintf(
        application_software_version,
        sizeof(application_software_version),
        "%.15s (%.31s)",
        app != NULL ? app->version : "unknown",
        diagnostics_git_revision());
    *state = (bacnet_device_state_t){
        .device_instance = bacnet_config.device_instance,
        .vendor_identifier = bacnet_config.vendor_identifier,
        .device_name = bacnet_config.device_name,
        .vendor_name = bacnet_config.vendor_name,
        .model_name = "Waveshare ESP32-P4-POE-ETH",
        .firmware_revision = app != NULL ? app->version : "unknown",
        .application_software_version = application_software_version,
        .description =
            "Read-only BACnet/IP Device exposing three physical toggle inputs",
        .location = bacnet_config.location,
        .database_revision = bacnet_config.database_revision +
            BACNET_FIRMWARE_DATABASE_REVISION_OFFSET,
        .binary_input_instances = {
            bacnet_config.input_instances[0],
            bacnet_config.input_instances[1],
            bacnet_config.input_instances[2],
            BACNET_ETHERNET_LINK_INPUT_INSTANCE,
            BACNET_IPV4_READY_INPUT_INSTANCE,
        },
        .binary_input_names = {
            bacnet_config.input_names[0],
            bacnet_config.input_names[1],
            bacnet_config.input_names[2],
            BACNET_ETHERNET_LINK_INPUT_NAME,
            BACNET_IPV4_READY_INPUT_NAME,
        },
        .binary_input_descriptions = {
            INPUT_DESCRIPTIONS[0],
            INPUT_DESCRIPTIONS[1],
            INPUT_DESCRIPTIONS[2],
            INPUT_DESCRIPTIONS[3],
            INPUT_DESCRIPTIONS[4],
        },
        .binary_input_values = {
            switch_input_get(0),
            switch_input_get(1),
            switch_input_get(2),
            diagnostics_valid && diagnostics.network.link_up,
            diagnostics_valid && diagnostics.network.ipv4_address != 0U,
        },
        .binary_input_reliability = {
            switch_input_faulted(0)
                ? BACNET_RELIABILITY_UNRELIABLE_OTHER
                : BACNET_RELIABILITY_NO_FAULT,
            switch_input_faulted(1)
                ? BACNET_RELIABILITY_UNRELIABLE_OTHER
                : BACNET_RELIABILITY_NO_FAULT,
            switch_input_faulted(2)
                ? BACNET_RELIABILITY_UNRELIABLE_OTHER
                : BACNET_RELIABILITY_NO_FAULT,
            diagnostics_valid
                ? BACNET_RELIABILITY_NO_FAULT
                : BACNET_RELIABILITY_UNRELIABLE_OTHER,
            diagnostics_valid
                ? BACNET_RELIABILITY_NO_FAULT
                : BACNET_RELIABILITY_UNRELIABLE_OTHER,
        },
        .binary_input_active_low = {
            switch_input_active_low(0),
            switch_input_active_low(1),
            switch_input_active_low(2),
            false,
            false,
        },
        .analog_value_instances = {
            ANALOG_VALUE_INSTANCES[0],
            ANALOG_VALUE_INSTANCES[1],
            ANALOG_VALUE_INSTANCES[2],
            ANALOG_VALUE_INSTANCES[3],
            ANALOG_VALUE_INSTANCES[4],
            ANALOG_VALUE_INSTANCES[5],
            ANALOG_VALUE_INSTANCES[6],
            ANALOG_VALUE_INSTANCES[7],
            ANALOG_VALUE_INSTANCES[8],
            ANALOG_VALUE_INSTANCES[9],
            ANALOG_VALUE_INSTANCES[10],
        },
        .analog_value_names = {
            ANALOG_VALUE_NAMES[0],
            ANALOG_VALUE_NAMES[1],
            ANALOG_VALUE_NAMES[2],
            ANALOG_VALUE_NAMES[3],
            ANALOG_VALUE_NAMES[4],
            ANALOG_VALUE_NAMES[5],
            ANALOG_VALUE_NAMES[6],
            ANALOG_VALUE_NAMES[7],
            ANALOG_VALUE_NAMES[8],
            ANALOG_VALUE_NAMES[9],
            ANALOG_VALUE_NAMES[10],
        },
        .analog_value_descriptions = {
            ANALOG_VALUE_DESCRIPTIONS[0],
            ANALOG_VALUE_DESCRIPTIONS[1],
            ANALOG_VALUE_DESCRIPTIONS[2],
            ANALOG_VALUE_DESCRIPTIONS[3],
            ANALOG_VALUE_DESCRIPTIONS[4],
            ANALOG_VALUE_DESCRIPTIONS[5],
            ANALOG_VALUE_DESCRIPTIONS[6],
            ANALOG_VALUE_DESCRIPTIONS[7],
            ANALOG_VALUE_DESCRIPTIONS[8],
            ANALOG_VALUE_DESCRIPTIONS[9],
            ANALOG_VALUE_DESCRIPTIONS[10],
        },
        .analog_value_values = {
            diagnostics_valid ? diagnostics.chip_temperature_c : 0.0F,
            diagnostics_valid ? diagnostics.uptime_ms / 1000.0F : 0.0F,
            diagnostics_valid ? (float)diagnostics.free_heap_bytes : 0.0F,
            diagnostics_valid
                ? (float)diagnostics.minimum_free_heap_bytes
                : 0.0F,
            diagnostics_valid
                ? (float)diagnostics.network.link_down_count
                : 0.0F,
            diagnostics_valid
                ? (float)diagnostics.network.reconnect_count
                : 0.0F,
            diagnostics_valid
                ? (float)diagnostics.bacnet[DIAGNOSTICS_BACNET_RX]
                : 0.0F,
            diagnostics_valid
                ? (float)(diagnostics.bacnet[DIAGNOSTICS_BACNET_ERRORS] +
                    diagnostics.bacnet[DIAGNOSTICS_BACNET_MALFORMED])
                : 0.0F,
            diagnostics_valid ? (float)diagnostics.reset_reason : 0.0F,
            diagnostics_valid
                ? (float)diagnostics.active_cov_subscriptions
                : 0.0F,
            diagnostics_valid ? (float)diagnostics.boot_count : 0.0F,
        },
        .analog_value_units = {
            ANALOG_VALUE_UNITS[0],
            ANALOG_VALUE_UNITS[1],
            ANALOG_VALUE_UNITS[2],
            ANALOG_VALUE_UNITS[3],
            ANALOG_VALUE_UNITS[4],
            ANALOG_VALUE_UNITS[5],
            ANALOG_VALUE_UNITS[6],
            ANALOG_VALUE_UNITS[7],
            ANALOG_VALUE_UNITS[8],
            ANALOG_VALUE_UNITS[9],
            ANALOG_VALUE_UNITS[10],
        },
        .analog_value_reliability = {
            diagnostics_valid && diagnostics.chip_temperature_valid
                ? BACNET_RELIABILITY_NO_FAULT
                : BACNET_RELIABILITY_UNRELIABLE_OTHER,
            diagnostics_valid
                ? BACNET_RELIABILITY_NO_FAULT
                : BACNET_RELIABILITY_UNRELIABLE_OTHER,
            diagnostics_valid
                ? BACNET_RELIABILITY_NO_FAULT
                : BACNET_RELIABILITY_UNRELIABLE_OTHER,
            diagnostics_valid
                ? BACNET_RELIABILITY_NO_FAULT
                : BACNET_RELIABILITY_UNRELIABLE_OTHER,
            diagnostics_valid
                ? BACNET_RELIABILITY_NO_FAULT
                : BACNET_RELIABILITY_UNRELIABLE_OTHER,
            diagnostics_valid
                ? BACNET_RELIABILITY_NO_FAULT
                : BACNET_RELIABILITY_UNRELIABLE_OTHER,
            diagnostics_valid
                ? BACNET_RELIABILITY_NO_FAULT
                : BACNET_RELIABILITY_UNRELIABLE_OTHER,
            diagnostics_valid
                ? BACNET_RELIABILITY_NO_FAULT
                : BACNET_RELIABILITY_UNRELIABLE_OTHER,
            diagnostics_valid
                ? BACNET_RELIABILITY_NO_FAULT
                : BACNET_RELIABILITY_UNRELIABLE_OTHER,
            diagnostics_valid
                ? BACNET_RELIABILITY_NO_FAULT
                : BACNET_RELIABILITY_UNRELIABLE_OTHER,
            diagnostics_valid
                ? BACNET_RELIABILITY_NO_FAULT
                : BACNET_RELIABILITY_UNRELIABLE_OTHER,
        },
        .network_port_instance = 1U,
        .network_port_name = "BACnet/IP Ethernet",
        .network_port_description =
            "Primary BACnet/IPv4 Ethernet interface",
        .network_port_reliability =
            diagnostics_valid && diagnostics.network.link_up &&
                diagnostics.network.ipv4_address != 0U
            ? BACNET_RELIABILITY_NO_FAULT
            : 12U, /* communication-failure */
        .network_port_out_of_service = false,
        .network_port_changes_pending =
            config_store_restart_required() ||
            network_config_restart_required(),
        .network_port_link_speed_bps =
            diagnostics_valid
            ? (float)diagnostics.network.speed_mbps * 1000000.0F
            : 0.0F,
        .network_port_udp_port = bacnet_config.bacnet_port,
        .network_port_dhcp_enabled =
            network_config.mode == NETWORK_ADDRESS_DHCP,
    };
    if (diagnostics_valid) {
        memcpy(
            state->network_port_ipv4,
            &diagnostics.network.ipv4_address,
            sizeof(state->network_port_ipv4));
        memcpy(
            state->network_port_netmask,
            &diagnostics.network.ipv4_netmask,
            sizeof(state->network_port_netmask));
        memcpy(
            state->network_port_gateway,
            &diagnostics.network.ipv4_gateway,
            sizeof(state->network_port_gateway));
        memcpy(
            state->network_port_dns,
            diagnostics.network.ipv4_dns,
            sizeof(state->network_port_dns));
    }
}

static bool valid_source(const struct sockaddr_in *source)
{
    if (source->sin_family != AF_INET) {
        return false;
    }
    const uint32_t address = ntohl(source->sin_addr.s_addr);
    return address != INADDR_ANY && address != INADDR_BROADCAST &&
        (address & 0xF0000000U) != 0xE0000000U;
}

static bool response_allowed(int64_t *window_started_us, uint32_t *responses)
{
    const int64_t now = esp_timer_get_time();
    if (now - *window_started_us >= 1000000LL) {
        *window_started_us = now;
        *responses = 0;
    }
    if (*responses >= BACNET_MAX_RESPONSES_PER_SECOND) {
        return false;
    }
    (*responses)++;
    return true;
}

static bool same_recipient(
    const struct sockaddr_in *left,
    const struct sockaddr_in *right)
{
    return left->sin_family == right->sin_family &&
        left->sin_port == right->sin_port &&
        left->sin_addr.s_addr == right->sin_addr.s_addr;
}

static uint32_t active_subscription_count(void)
{
    uint32_t count = 0U;
    for (size_t index = 0; index < BACNET_MAX_COV_SUBSCRIPTIONS; ++index) {
        if (cov_subscriptions[index].active) {
            count++;
        }
    }
    return count;
}

static void update_active_subscription_diagnostic(void)
{
    diagnostics_bacnet_set_active_subscriptions(
        active_subscription_count());
}

static bool input_index_for_instance(
    const bacnet_device_state_t *state,
    uint32_t instance,
    size_t *input_index)
{
    for (size_t index = 0; index < BACNET_BINARY_INPUT_COUNT; ++index) {
        if (state->binary_input_instances[index] == instance) {
            *input_index = index;
            return true;
        }
    }
    return false;
}

static bool cov_subscription_can_apply(
    const bacnet_packet_result_t *packet,
    const struct sockaddr_in *source,
    const bacnet_device_state_t *state)
{
    if (packet->cov_cancel) {
        return true;
    }
    size_t input_index = 0U;
    if (!input_index_for_instance(
            state, packet->cov_object_instance, &input_index)) {
        return true;
    }
    for (size_t index = 0; index < BACNET_MAX_COV_SUBSCRIPTIONS; ++index) {
        const cov_subscription_t *subscription = &cov_subscriptions[index];
        if (!subscription->active ||
            (same_recipient(&subscription->recipient, source) &&
             subscription->process_id == packet->cov_process_id &&
             subscription->input_index == input_index)) {
            return true;
        }
    }
    return false;
}

static void apply_cov_subscription(
    const bacnet_packet_result_t *packet,
    const struct sockaddr_in *source,
    const bacnet_device_state_t *state)
{
    size_t input_index = 0U;
    if (!input_index_for_instance(
            state, packet->cov_object_instance, &input_index)) {
        return;
    }

    for (size_t index = 0; index < BACNET_MAX_COV_SUBSCRIPTIONS; ++index) {
        cov_subscription_t *subscription = &cov_subscriptions[index];
        if (subscription->active &&
            same_recipient(&subscription->recipient, source) &&
            subscription->process_id == packet->cov_process_id &&
            subscription->input_index == input_index) {
            if (packet->cov_cancel) {
                memset(subscription, 0, sizeof(*subscription));
                update_active_subscription_diagnostic();
                return;
            }
            subscription->confirmed = packet->cov_confirmed;
            const uint32_t lifetime =
                packet->cov_lifetime_seconds >
                    BACNET_COV_MAX_LIFETIME_SECONDS
                ? BACNET_COV_MAX_LIFETIME_SECONDS
                : packet->cov_lifetime_seconds;
            subscription->expires_at_us = lifetime == 0U
                ? 0LL
                : esp_timer_get_time() + (int64_t)lifetime * 1000000LL;
            subscription->dirty = true;
            subscription->awaiting_ack = false;
            subscription->retry_count = 0U;
            cov_retry_cache_clear(&subscription->retry_payload);
            return;
        }
    }
    if (packet->cov_cancel) {
        return;
    }

    size_t selected = BACNET_MAX_COV_SUBSCRIPTIONS;
    for (size_t index = 0; index < BACNET_MAX_COV_SUBSCRIPTIONS; ++index) {
        if (!cov_subscriptions[index].active) {
            selected = index;
            break;
        }
    }
    if (selected >= BACNET_MAX_COV_SUBSCRIPTIONS) {
        return;
    }

    const uint32_t lifetime =
        packet->cov_lifetime_seconds > BACNET_COV_MAX_LIFETIME_SECONDS
        ? BACNET_COV_MAX_LIFETIME_SECONDS
        : packet->cov_lifetime_seconds;
    cov_subscriptions[selected] = (cov_subscription_t){
        .active = true,
        .recipient = *source,
        .process_id = packet->cov_process_id,
        .input_index = input_index,
        .confirmed = packet->cov_confirmed,
        .expires_at_us = lifetime == 0U
            ? 0LL
            : esp_timer_get_time() + (int64_t)lifetime * 1000000LL,
        .dirty = true,
    };
    update_active_subscription_diagnostic();
}

static void handle_cov_ack(
    const bacnet_packet_result_t *packet,
    const struct sockaddr_in *source)
{
    for (size_t index = 0; index < BACNET_MAX_COV_SUBSCRIPTIONS; ++index) {
        cov_subscription_t *subscription = &cov_subscriptions[index];
        if (!subscription->active || !subscription->awaiting_ack ||
            subscription->invoke_id != packet->invoke_id ||
            !same_recipient(&subscription->recipient, source)) {
            continue;
        }
        subscription->awaiting_ack = false;
        subscription->retry_count = 0U;
        cov_retry_cache_clear(&subscription->retry_payload);
        if (packet->cov_ack_error) {
            diagnostics_bacnet_increment(DIAGNOSTICS_BACNET_ERRORS);
            memset(subscription, 0, sizeof(*subscription));
            update_active_subscription_diagnostic();
        } else {
            diagnostics_bacnet_increment(DIAGNOSTICS_BACNET_COV_ACKED);
        }
        return;
    }
}

static uint32_t cov_time_remaining_seconds(
    const cov_subscription_t *subscription,
    int64_t now_us)
{
    if (subscription->expires_at_us == 0LL) {
        return 0U;
    }
    if (subscription->expires_at_us <= now_us) {
        return 0U;
    }
    const int64_t remaining = subscription->expires_at_us - now_us;
    return (uint32_t)((remaining + 999999LL) / 1000000LL);
}

static void service_cov_subscriptions(
    int socket_fd,
    const bacnet_device_state_t *state,
    bool last_values[BACNET_BINARY_INPUT_COUNT],
    uint32_t last_reliability[BACNET_BINARY_INPUT_COUNT])
{
    const int64_t now_us = esp_timer_get_time();
    for (size_t input = 0; input < BACNET_BINARY_INPUT_COUNT; ++input) {
        if (last_values[input] == state->binary_input_values[input] &&
            last_reliability[input] ==
                state->binary_input_reliability[input]) {
            continue;
        }
        last_values[input] = state->binary_input_values[input];
        last_reliability[input] = state->binary_input_reliability[input];
        for (size_t index = 0;
             index < BACNET_MAX_COV_SUBSCRIPTIONS;
             ++index) {
            if (cov_subscriptions[index].active &&
                cov_subscriptions[index].input_index == input) {
                cov_subscriptions[index].dirty = true;
            }
        }
    }

    uint8_t notification[BACNET_RESPONSE_CAPACITY];
    for (size_t index = 0; index < BACNET_MAX_COV_SUBSCRIPTIONS; ++index) {
        cov_subscription_t *subscription = &cov_subscriptions[index];
        if (!subscription->active) {
            continue;
        }
        if (subscription->expires_at_us != 0LL &&
            subscription->expires_at_us <= now_us) {
            memset(subscription, 0, sizeof(*subscription));
            continue;
        }

        bool retry = false;
        if (subscription->awaiting_ack) {
            if (now_us < subscription->next_retry_us) {
                continue;
            }
            if (subscription->retry_count >= BACNET_COV_MAX_RETRIES) {
                diagnostics_bacnet_increment(
                    DIAGNOSTICS_BACNET_COV_TIMEOUTS);
                diagnostics_bacnet_increment(DIAGNOSTICS_BACNET_ERRORS);
                memset(subscription, 0, sizeof(*subscription));
                continue;
            }
            retry = true;
            subscription->retry_count++;
        } else if (!subscription->dirty) {
            continue;
        }

        if (subscription->confirmed && !retry) {
            subscription->invoke_id = next_cov_invoke_id++;
            if (next_cov_invoke_id == 0U) {
                next_cov_invoke_id = 1U;
            }
            subscription->retry_count = 0U;
        }
        const uint8_t *payload = notification;
        size_t length = 0U;
        if (subscription->confirmed && retry) {
            payload = cov_retry_cache_data(&subscription->retry_payload);
            length = cov_retry_cache_length(&subscription->retry_payload);
            if (payload == NULL || length == 0U) {
                diagnostics_bacnet_increment(DIAGNOSTICS_BACNET_ERRORS);
                memset(subscription, 0, sizeof(*subscription));
                continue;
            }
        } else {
            length = bacnet_encode_cov_notification(
                state,
                subscription->input_index,
                subscription->process_id,
                cov_time_remaining_seconds(subscription, now_us),
                subscription->confirmed,
                subscription->invoke_id,
                notification,
                sizeof(notification));
            if (subscription->confirmed && length > 0U &&
                !cov_retry_cache_capture(
                    &subscription->retry_payload, notification, length)) {
                diagnostics_bacnet_increment(DIAGNOSTICS_BACNET_ERRORS);
                continue;
            }
        }
        if (length == 0U ||
            sendto(
                socket_fd,
                payload,
                length,
                0,
                (const struct sockaddr *)&subscription->recipient,
                sizeof(subscription->recipient)) < 0) {
            diagnostics_bacnet_increment(DIAGNOSTICS_BACNET_ERRORS);
            if (subscription->confirmed && retry) {
                subscription->next_retry_us =
                    now_us + (int64_t)BACNET_COV_RETRY_MS * 1000LL;
            }
            continue;
        }
        diagnostics_bacnet_increment(DIAGNOSTICS_BACNET_COV_SENT);
        if (!retry) {
            subscription->dirty = false;
        }
        if (subscription->confirmed) {
            subscription->awaiting_ack = true;
            subscription->next_retry_us =
                now_us + (int64_t)BACNET_COV_RETRY_MS * 1000LL;
        }
    }
    update_active_subscription_diagnostic();
}

static bool broadcast_destination(
    esp_netif_t *netif,
    struct sockaddr_in *destination)
{
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK ||
        ip_info.ip.addr == 0U || ip_info.netmask.addr == 0U) {
        return false;
    }

    *destination = (struct sockaddr_in){
        .sin_family = AF_INET,
        .sin_port = htons(bacnet_config.bacnet_port),
        .sin_addr.s_addr =
            (ip_info.ip.addr & ip_info.netmask.addr) | ~ip_info.netmask.addr,
    };
    return true;
}

static void send_i_am_broadcast(int socket_fd, esp_netif_t *netif)
{
    struct sockaddr_in destination;
    if (!broadcast_destination(netif, &destination)) {
        ESP_LOGW(TAG, "cannot announce I-Am before IPv4 configuration");
        return;
    }

    bacnet_device_state_t state;
    uint8_t response[BACNET_RESPONSE_CAPACITY];
    snapshot_device_state(&state);
    const size_t length =
        bacnet_encode_i_am(&state, true, response, sizeof(response));
    if (length == 0U) {
        ESP_LOGE(TAG, "failed to encode startup I-Am");
        return;
    }

    if (sendto(
            socket_fd,
            response,
            length,
            0,
            (const struct sockaddr *)&destination,
            sizeof(destination)) < 0) {
        ESP_LOGW(TAG, "startup I-Am send failed: errno %d", errno);
        return;
    }
    ESP_LOGI(
        TAG,
        "announced Device %u on UDP port %u",
        (unsigned)state.device_instance,
        (unsigned)bacnet_config.bacnet_port);
}

static int open_bacnet_socket(void)
{
    const int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0) {
        ESP_LOGE(TAG, "socket creation failed: errno %d", errno);
        return -1;
    }

    const int enabled = 1;
    const struct timeval timeout = {
        .tv_sec = 0,
        .tv_usec = BACNET_RECEIVE_TIMEOUT_US,
    };
    if (setsockopt(
            socket_fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0 ||
        setsockopt(
            socket_fd, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) < 0 ||
        setsockopt(
            socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        ESP_LOGE(TAG, "socket option setup failed: errno %d", errno);
        close(socket_fd);
        return -1;
    }

    const struct sockaddr_in bind_address = {
        .sin_family = AF_INET,
        .sin_port = htons(bacnet_config.bacnet_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(
            socket_fd,
            (const struct sockaddr *)&bind_address,
            sizeof(bind_address)) < 0) {
        ESP_LOGE(
            TAG,
            "bind to UDP %u failed: errno %d",
            (unsigned)bacnet_config.bacnet_port,
            errno);
        close(socket_fd);
        return -1;
    }
    return socket_fd;
}

static void bacnet_server_task(void *argument)
{
    esp_netif_t *netif = argument;
    uint8_t request[BACNET_MAX_REQUEST_BYTES + 1U];
    uint8_t response[BACNET_RESPONSE_CAPACITY];
    bool socket_failure_recorded = false;
    memset(cov_subscriptions, 0, sizeof(cov_subscriptions));
    diagnostics_bacnet_set_active_subscriptions(0U);
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        diagnostics_task_watchdog_subscribe(DIAGNOSTICS_TASK_BACNET));

    for (;;) {
        diagnostics_task_heartbeat(DIAGNOSTICS_TASK_BACNET);
        const int socket_fd = open_bacnet_socket();
        if (socket_fd < 0) {
            atomic_store_explicit(&server_ready, false, memory_order_release);
            if (!socket_failure_recorded) {
                diagnostics_record_event(
                    DIAGNOSTICS_EVENT_BACNET_SOCKET_FAILED, errno);
                socket_failure_recorded = true;
            }
            vTaskDelay(pdMS_TO_TICKS(BACNET_SOCKET_RETRY_MS));
            continue;
        }
        socket_failure_recorded = false;
        atomic_store_explicit(&server_ready, true, memory_order_release);

        int64_t window_started_us = esp_timer_get_time();
        uint32_t responses_in_window = 0;
        send_i_am_broadcast(socket_fd, netif);
        bacnet_device_state_t initial_state;
        snapshot_device_state(&initial_state);
        bool last_values[BACNET_BINARY_INPUT_COUNT];
        uint32_t last_reliability[BACNET_BINARY_INPUT_COUNT];
        memcpy(
            last_values,
            initial_state.binary_input_values,
            sizeof(last_values));
        memcpy(
            last_reliability,
            initial_state.binary_input_reliability,
            sizeof(last_reliability));

        for (;;) {
            diagnostics_task_heartbeat(DIAGNOSTICS_TASK_BACNET);
            if (ulTaskNotifyTake(pdTRUE, 0) != 0U) {
                send_i_am_broadcast(socket_fd, netif);
            }

            bacnet_device_state_t cov_state;
            snapshot_device_state(&cov_state);
            service_cov_subscriptions(
                socket_fd, &cov_state, last_values, last_reliability);

            struct sockaddr_in source;
            socklen_t source_length = sizeof(source);
            const ssize_t received = recvfrom(
                socket_fd,
                request,
                sizeof(request),
                0,
                (struct sockaddr *)&source,
                &source_length);
            if (received < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                ESP_LOGW(TAG, "UDP receive failed: errno %d; reopening", errno);
                break;
            }
            if (!valid_source(&source)) {
                continue;
            }
            diagnostics_bacnet_increment(DIAGNOSTICS_BACNET_RX);

            bacnet_device_state_t state;
            snapshot_device_state(&state);
            bacnet_packet_result_t packet = bacnet_handle_packet(
                request,
                (size_t)received,
                &state,
                response,
                sizeof(response));
            bool apply_cov = packet.kind == BACNET_PACKET_SUBSCRIBE_COV &&
                packet.response_length > 6U && response[6] == 0x20U;
            if (apply_cov &&
                !cov_subscription_can_apply(&packet, &source, &state)) {
                packet.response_length = bacnet_encode_subscribe_cov_no_space(
                    packet.invoke_id, response, sizeof(response));
                apply_cov = false;
            }
            switch (packet.kind) {
            case BACNET_PACKET_WHO_IS:
                diagnostics_bacnet_increment(DIAGNOSTICS_BACNET_WHO_IS);
                break;
            case BACNET_PACKET_WHO_HAS:
                diagnostics_bacnet_increment(DIAGNOSTICS_BACNET_WHO_HAS);
                break;
            case BACNET_PACKET_READ_PROPERTY:
                diagnostics_bacnet_increment(
                    DIAGNOSTICS_BACNET_READ_PROPERTY);
                break;
            case BACNET_PACKET_READ_PROPERTY_MULTIPLE:
                diagnostics_bacnet_increment(
                    DIAGNOSTICS_BACNET_READ_PROPERTY_MULTIPLE);
                break;
            case BACNET_PACKET_SUBSCRIBE_COV:
                diagnostics_bacnet_increment(
                    DIAGNOSTICS_BACNET_SUBSCRIBE_COV);
                break;
            case BACNET_PACKET_COV_ACK:
                handle_cov_ack(&packet, &source);
                break;
            case BACNET_PACKET_MALFORMED:
                diagnostics_bacnet_increment(
                    DIAGNOSTICS_BACNET_MALFORMED);
                break;
            case BACNET_PACKET_IGNORED:
            default:
                diagnostics_bacnet_increment(DIAGNOSTICS_BACNET_IGNORED);
                break;
            }
            if (packet.response_length == 0U) {
                continue;
            }
            if (!response_allowed(
                    &window_started_us, &responses_in_window)) {
                diagnostics_bacnet_increment(
                    DIAGNOSTICS_BACNET_RATE_LIMITED);
                continue;
            }
            struct sockaddr_in destination = source;
            socklen_t destination_length = source_length;
            if (packet.broadcast_response) {
                if (!broadcast_destination(netif, &destination)) {
                    ESP_LOGW(
                        TAG,
                        "cannot send BACnet broadcast response without IPv4 configuration");
                    continue;
                }
                destination_length = sizeof(destination);
            }
            const ssize_t sent = sendto(
                    socket_fd,
                    response,
                    packet.response_length,
                    0,
                    (const struct sockaddr *)&destination,
                    destination_length);
            if (sent < 0) {
                ESP_LOGW(TAG, "UDP response failed: errno %d", errno);
                diagnostics_bacnet_increment(DIAGNOSTICS_BACNET_ERRORS);
                continue;
            }
            diagnostics_bacnet_increment(DIAGNOSTICS_BACNET_RESPONSES);
            if (response[6] == 0x50U || response[6] == 0x60U ||
                response[6] == 0x70U || response[6] == 0x71U) {
                diagnostics_bacnet_increment(DIAGNOSTICS_BACNET_ERRORS);
            }
            if (apply_cov) {
                apply_cov_subscription(&packet, &source, &state);
            }
        }
        atomic_store_explicit(&server_ready, false, memory_order_release);
        close(socket_fd);
        vTaskDelay(pdMS_TO_TICKS(BACNET_SOCKET_RETRY_MS));
    }
}

bool bacnet_server_ready(void)
{
    return atomic_load_explicit(&server_ready, memory_order_acquire);
}

esp_err_t bacnet_server_start(esp_netif_t *netif)
{
    ESP_RETURN_ON_FALSE(netif != NULL, ESP_ERR_INVALID_ARG, TAG, "netif is null");
    config_store_get_active(&bacnet_config);
    ESP_RETURN_ON_FALSE(
        config_model_is_valid_blob(&bacnet_config) &&
            bacnet_config.device_instance < BACNET_MAX_INSTANCE,
        ESP_ERR_INVALID_ARG,
        TAG,
        "invalid BACnet Device instance");
    const uint32_t input_instances[BACNET_BINARY_INPUT_COUNT] = {
        bacnet_config.input_instances[0],
        bacnet_config.input_instances[1],
        bacnet_config.input_instances[2],
        BACNET_ETHERNET_LINK_INPUT_INSTANCE,
        BACNET_IPV4_READY_INPUT_INSTANCE,
    };
    for (size_t index = 0; index < BACNET_BINARY_INPUT_COUNT; ++index) {
        ESP_RETURN_ON_FALSE(
            input_instances[index] < BACNET_MAX_INSTANCE,
            ESP_ERR_INVALID_ARG,
            TAG,
            "invalid Binary Input instance");
        for (size_t prior = 0; prior < index; ++prior) {
            ESP_RETURN_ON_FALSE(
                input_instances[index] != input_instances[prior],
                ESP_ERR_INVALID_ARG,
                TAG,
                "Binary Input instances must be distinct");
        }
    }

    taskENTER_CRITICAL(&server_lock);
    TaskHandle_t existing_task = server_task_handle;
    taskEXIT_CRITICAL(&server_lock);
    if (existing_task != NULL) {
        xTaskNotifyGive(existing_task);
        return ESP_OK;
    }

    atomic_store_explicit(&server_ready, false, memory_order_release);
    TaskHandle_t created_task = NULL;
    const BaseType_t created = xTaskCreate(
        bacnet_server_task,
        "bacnet_ip",
        12288,
        netif,
        5,
        &created_task);
    ESP_RETURN_ON_FALSE(
        created == pdPASS,
        ESP_ERR_NO_MEM,
        TAG,
        "failed to create BACnet/IP task");

    taskENTER_CRITICAL(&server_lock);
    server_task_handle = created_task;
    taskEXIT_CRITICAL(&server_lock);
    return ESP_OK;
}
