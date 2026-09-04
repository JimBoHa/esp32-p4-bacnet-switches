#include "bacnet_server.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bacnet_codec.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include "switch_inputs.h"

#define BACNET_RESPONSE_CAPACITY (BACNET_MAX_APDU + 6U)
#define BACNET_MAX_RESPONSES_PER_SECOND 50U
#define BACNET_SOCKET_RETRY_MS 1000U

static const char *TAG = "bacnet_ip";
_Static_assert(
    BACNET_BINARY_INPUT_COUNT == SWITCH_INPUT_COUNT,
    "BACnet and physical input counts must match");
static const char *const INPUT_DESCRIPTIONS[BACNET_BINARY_INPUT_COUNT] = {
    "Debounced read-only physical toggle input 1",
    "Debounced read-only physical toggle input 2",
    "Debounced read-only physical toggle input 3",
};

static TaskHandle_t server_task_handle;
static portMUX_TYPE server_lock = portMUX_INITIALIZER_UNLOCKED;

static void snapshot_device_state(bacnet_device_state_t *state)
{
    const esp_app_desc_t *app = esp_app_get_description();
    *state = (bacnet_device_state_t){
        .device_instance = CONFIG_BACNET_DEVICE_INSTANCE,
        .vendor_identifier = CONFIG_BACNET_VENDOR_IDENTIFIER,
        .device_name = CONFIG_BACNET_DEVICE_NAME,
        .vendor_name = CONFIG_BACNET_VENDOR_NAME,
        .model_name = "Waveshare ESP32-P4-POE-ETH",
        .firmware_revision = app != NULL ? app->version : "unknown",
        .description =
            "Read-only BACnet/IP Device exposing three physical toggle inputs",
        .database_revision = 2,
        .binary_input_instances = {
            CONFIG_TOGGLE_INPUT_1_OBJECT_INSTANCE,
            CONFIG_TOGGLE_INPUT_2_OBJECT_INSTANCE,
            CONFIG_TOGGLE_INPUT_3_OBJECT_INSTANCE,
        },
        .binary_input_names = {
            CONFIG_TOGGLE_INPUT_1_NAME,
            CONFIG_TOGGLE_INPUT_2_NAME,
            CONFIG_TOGGLE_INPUT_3_NAME,
        },
        .binary_input_descriptions = {
            INPUT_DESCRIPTIONS[0],
            INPUT_DESCRIPTIONS[1],
            INPUT_DESCRIPTIONS[2],
        },
        .binary_input_values = {
            switch_input_get(0),
            switch_input_get(1),
            switch_input_get(2),
        },
    };
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
        .sin_port = htons(CONFIG_BACNET_UDP_PORT),
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
        (unsigned)CONFIG_BACNET_UDP_PORT);
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
        .tv_sec = 1,
        .tv_usec = 0,
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
        .sin_port = htons(CONFIG_BACNET_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(
            socket_fd,
            (const struct sockaddr *)&bind_address,
            sizeof(bind_address)) < 0) {
        ESP_LOGE(
            TAG,
            "bind to UDP %u failed: errno %d",
            (unsigned)CONFIG_BACNET_UDP_PORT,
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

    for (;;) {
        const int socket_fd = open_bacnet_socket();
        if (socket_fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(BACNET_SOCKET_RETRY_MS));
            continue;
        }

        int64_t window_started_us = esp_timer_get_time();
        uint32_t responses_in_window = 0;
        send_i_am_broadcast(socket_fd, netif);

        for (;;) {
            if (ulTaskNotifyTake(pdTRUE, 0) != 0U) {
                send_i_am_broadcast(socket_fd, netif);
            }

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

            bacnet_device_state_t state;
            snapshot_device_state(&state);
            const bacnet_packet_result_t packet = bacnet_handle_packet(
                request,
                (size_t)received,
                &state,
                response,
                sizeof(response));
            if (packet.response_length == 0U ||
                !response_allowed(
                    &window_started_us, &responses_in_window)) {
                continue;
            }
            struct sockaddr_in destination = source;
            socklen_t destination_length = source_length;
            if (packet.broadcast_response) {
                if (!broadcast_destination(netif, &destination)) {
                    ESP_LOGW(TAG, "cannot send I-Am without IPv4 configuration");
                    continue;
                }
                destination_length = sizeof(destination);
            }
            if (sendto(
                    socket_fd,
                    response,
                    packet.response_length,
                    0,
                    (const struct sockaddr *)&destination,
                    destination_length) < 0) {
                ESP_LOGW(TAG, "UDP response failed: errno %d", errno);
            }
        }
        close(socket_fd);
        vTaskDelay(pdMS_TO_TICKS(BACNET_SOCKET_RETRY_MS));
    }
}

esp_err_t bacnet_server_start(esp_netif_t *netif)
{
    ESP_RETURN_ON_FALSE(netif != NULL, ESP_ERR_INVALID_ARG, TAG, "netif is null");
    ESP_RETURN_ON_FALSE(
        CONFIG_BACNET_DEVICE_INSTANCE < BACNET_MAX_INSTANCE,
        ESP_ERR_INVALID_ARG,
        TAG,
        "invalid BACnet Device instance");
    const uint32_t input_instances[BACNET_BINARY_INPUT_COUNT] = {
        CONFIG_TOGGLE_INPUT_1_OBJECT_INSTANCE,
        CONFIG_TOGGLE_INPUT_2_OBJECT_INSTANCE,
        CONFIG_TOGGLE_INPUT_3_OBJECT_INSTANCE,
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

    TaskHandle_t created_task = NULL;
    const BaseType_t created = xTaskCreate(
        bacnet_server_task,
        "bacnet_ip",
        8192,
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
