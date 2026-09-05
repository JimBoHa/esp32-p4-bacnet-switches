#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "diagnostics_metrics.h"

#define DIAGNOSTICS_FAULT_LOG_CAPACITY 16U
#define DIAGNOSTICS_TASK_COUNT 2U
#define DIAGNOSTICS_TEMPERATURE_SAMPLE_INTERVAL_MS 1000U

typedef enum {
    DIAGNOSTICS_EVENT_BOOT = 1,
    DIAGNOSTICS_EVENT_ABNORMAL_RESET,
    DIAGNOSTICS_EVENT_ETHERNET_LINK_LOST,
    DIAGNOSTICS_EVENT_IP_LOST,
    DIAGNOSTICS_EVENT_OTA_ACCEPTED,
    DIAGNOSTICS_EVENT_OTA_FAILED,
    DIAGNOSTICS_EVENT_OTA_VALIDATED,
    DIAGNOSTICS_EVENT_INPUT_SELF_TEST_FAILED,
    DIAGNOSTICS_EVENT_TEMPERATURE_SENSOR_FAILED,
    DIAGNOSTICS_EVENT_BACNET_SOCKET_FAILED,
    DIAGNOSTICS_EVENT_TASK_WATCHDOG_FAILED,
    DIAGNOSTICS_EVENT_OTA_SERVER_FAILED,
    DIAGNOSTICS_EVENT_REMOTE_REBOOT_REQUESTED,
    DIAGNOSTICS_EVENT_MDNS_FAILED,
} diagnostics_event_type_t;

typedef enum {
    DIAGNOSTICS_OTA_NONE,
    DIAGNOSTICS_OTA_ACCEPTED,
    DIAGNOSTICS_OTA_FAILED,
    DIAGNOSTICS_OTA_VALIDATED,
} diagnostics_ota_result_t;

typedef enum {
    DIAGNOSTICS_TASK_SWITCH_INPUTS,
    DIAGNOSTICS_TASK_BACNET,
} diagnostics_task_t;

typedef enum {
    DIAGNOSTICS_BACNET_RX,
    DIAGNOSTICS_BACNET_WHO_IS,
    DIAGNOSTICS_BACNET_WHO_HAS,
    DIAGNOSTICS_BACNET_READ_PROPERTY,
    DIAGNOSTICS_BACNET_READ_PROPERTY_MULTIPLE,
    DIAGNOSTICS_BACNET_SUBSCRIBE_COV,
    DIAGNOSTICS_BACNET_MALFORMED,
    DIAGNOSTICS_BACNET_IGNORED,
    DIAGNOSTICS_BACNET_RESPONSES,
    DIAGNOSTICS_BACNET_ERRORS,
    DIAGNOSTICS_BACNET_RATE_LIMITED,
    DIAGNOSTICS_BACNET_COV_SENT,
    DIAGNOSTICS_BACNET_COV_ACKED,
    DIAGNOSTICS_BACNET_COV_TIMEOUTS,
    DIAGNOSTICS_BACNET_COUNTER_COUNT,
} diagnostics_bacnet_counter_t;

typedef struct {
    uint32_t sequence;
    uint32_t boot_count;
    uint64_t uptime_ms;
    uint16_t type;
    int16_t code;
} diagnostics_fault_event_t;

typedef struct {
    bool link_up;
    uint32_t speed_mbps;
    bool full_duplex;
    bool autonegotiation;
    uint8_t mac[6];
    uint32_t link_up_count;
    uint32_t link_down_count;
    uint32_t reconnect_count;
    uint32_t ip_acquisition_count;
    uint32_t ip_changed_count;
    uint64_t ip_acquired_uptime_ms;
    uint32_t ipv4_address;
    uint32_t ipv4_netmask;
    uint32_t ipv4_gateway;
    uint32_t ipv4_dns[3];
    uint32_t dhcp_status;
} diagnostics_network_snapshot_t;

typedef struct {
    uint64_t uptime_ms;
    float chip_temperature_c;
    bool chip_temperature_valid;
    diagnostics_temperature_metrics_t temperature_metrics;
    uint64_t temperature_last_sample_uptime_ms;
    uint32_t free_heap_bytes;
    uint32_t minimum_free_heap_bytes;
    esp_reset_reason_t reset_reason;
    uint32_t boot_count;
    diagnostics_ota_result_t last_ota_result;
    diagnostics_network_snapshot_t network;
    uint32_t bacnet[DIAGNOSTICS_BACNET_COUNTER_COUNT];
    uint32_t active_cov_subscriptions;
    bool task_watchdog_subscribed[DIAGNOSTICS_TASK_COUNT];
    bool task_healthy[DIAGNOSTICS_TASK_COUNT];
    uint64_t task_last_heartbeat_ms[DIAGNOSTICS_TASK_COUNT];
    diagnostics_fault_event_t fault_log[DIAGNOSTICS_FAULT_LOG_CAPACITY];
    size_t fault_log_count;
    diagnostics_fault_log_metrics_t fault_log_metrics;
    bool persistent_storage_ready;
    uint32_t persistent_write_failure_count;
    int32_t persistent_last_write_error;
} diagnostics_snapshot_t;

esp_err_t diagnostics_init(void);
const char *diagnostics_git_revision(void);
const char *diagnostics_reset_reason_name(esp_reset_reason_t reason);
const char *diagnostics_event_name(diagnostics_event_type_t event);
const char *diagnostics_ota_result_name(diagnostics_ota_result_t result);

void diagnostics_record_event(diagnostics_event_type_t type, int code);
void diagnostics_record_ota_result(diagnostics_ota_result_t result, int code);
void diagnostics_set_ethernet_handle(esp_eth_handle_t handle);
void diagnostics_ethernet_link_changed(bool link_up);
void diagnostics_ip_acquired(
    esp_netif_t *netif,
    const esp_netif_ip_info_t *ip_info);
void diagnostics_ip_lost(void);

void diagnostics_bacnet_increment(diagnostics_bacnet_counter_t counter);
void diagnostics_bacnet_set_active_subscriptions(uint32_t count);

esp_err_t diagnostics_task_watchdog_subscribe(diagnostics_task_t task);
void diagnostics_task_heartbeat(diagnostics_task_t task);

bool diagnostics_snapshot_get(diagnostics_snapshot_t *snapshot);
