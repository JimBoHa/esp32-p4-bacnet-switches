#include "diagnostics.h"

#include <stdatomic.h>
#include <string.h>

#include "driver/temperature_sensor.h"
#include "diagnostics_time.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#ifndef PROJECT_GIT_REVISION
#define PROJECT_GIT_REVISION "unknown"
#endif

#define DIAGNOSTICS_PERSISTENT_MAGIC 0x44494147U
#define DIAGNOSTICS_PERSISTENT_VERSION 2U
#define DIAGNOSTICS_PERSISTENT_VERSION_LEGACY_32_BIT_TIME 1U
#define DIAGNOSTICS_NVS_NAMESPACE "diagnostics"
#define DIAGNOSTICS_NVS_KEY "state"
#define DIAGNOSTICS_TASK_HEALTH_MAX_AGE_MS 2500U

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t boot_count;
    uint32_t next_sequence;
    uint32_t event_head;
    uint32_t event_count;
    uint32_t last_ota_result;
    diagnostics_fault_event_t events[DIAGNOSTICS_FAULT_LOG_CAPACITY];
} diagnostics_persistent_state_t;

typedef struct {
    uint32_t sequence;
    uint32_t boot_count;
    uint32_t uptime_ms;
    uint16_t type;
    int16_t code;
} diagnostics_fault_event_v1_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t boot_count;
    uint32_t next_sequence;
    uint32_t event_head;
    uint32_t event_count;
    uint32_t last_ota_result;
    diagnostics_fault_event_v1_t events[DIAGNOSTICS_FAULT_LOG_CAPACITY];
} diagnostics_persistent_state_v1_t;

_Static_assert(
    sizeof(diagnostics_fault_event_v1_t) == 16U,
    "legacy diagnostics event layout changed");

static const char *TAG = "diagnostics";
static SemaphoreHandle_t state_mutex;
static nvs_handle_t diagnostics_nvs_handle;
static bool nvs_ready;
static diagnostics_persistent_state_t persistent;
static diagnostics_network_snapshot_t network_state;
static uint32_t last_acquired_ipv4_address;
static esp_eth_handle_t ethernet_handle;
static esp_reset_reason_t startup_reset_reason;
static temperature_sensor_handle_t temperature_sensor;
static atomic_bool temperature_read_failure_recorded;
static atomic_uint_fast32_t bacnet_counters[DIAGNOSTICS_BACNET_COUNTER_COUNT];
static atomic_uint_fast32_t active_cov_subscriptions;
static uint64_t task_last_heartbeat[DIAGNOSTICS_TASK_COUNT];
static portMUX_TYPE task_heartbeat_lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool task_watchdog_subscribed[DIAGNOSTICS_TASK_COUNT];

static uint64_t uptime_ms(void)
{
    return diagnostics_milliseconds_from_microseconds(esp_timer_get_time());
}

static bool abnormal_reset(esp_reset_reason_t reason)
{
    return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
        reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT ||
        reason == ESP_RST_BROWNOUT || reason == ESP_RST_EFUSE ||
        reason == ESP_RST_PWR_GLITCH || reason == ESP_RST_CPU_LOCKUP;
}

static void save_persistent_locked(void)
{
    if (!nvs_ready) {
        return;
    }
    const esp_err_t set_result = nvs_set_blob(
        diagnostics_nvs_handle,
        DIAGNOSTICS_NVS_KEY,
        &persistent,
        sizeof(persistent));
    const esp_err_t commit_result =
        set_result == ESP_OK ? nvs_commit(diagnostics_nvs_handle) : set_result;
    if (commit_result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "failed to persist diagnostics: %s",
            esp_err_to_name(commit_result));
    }
}

static bool persistent_state_header_valid(
    uint32_t magic,
    uint32_t version,
    uint32_t event_head,
    uint32_t event_count,
    uint32_t expected_version)
{
    return magic == DIAGNOSTICS_PERSISTENT_MAGIC &&
        version == expected_version &&
        event_head < DIAGNOSTICS_FAULT_LOG_CAPACITY &&
        event_count <= DIAGNOSTICS_FAULT_LOG_CAPACITY;
}

static bool load_persistent_state(void)
{
    size_t stored_size = 0U;
    esp_err_t result = nvs_get_blob(
        diagnostics_nvs_handle, DIAGNOSTICS_NVS_KEY, NULL, &stored_size);
    if (result != ESP_OK) {
        return false;
    }

    if (stored_size == sizeof(persistent)) {
        diagnostics_persistent_state_t stored = {0};
        result = nvs_get_blob(
            diagnostics_nvs_handle,
            DIAGNOSTICS_NVS_KEY,
            &stored,
            &stored_size);
        if (result == ESP_OK && persistent_state_header_valid(
                stored.magic,
                stored.version,
                stored.event_head,
                stored.event_count,
                DIAGNOSTICS_PERSISTENT_VERSION)) {
            persistent = stored;
            return true;
        }
        return false;
    }

    if (stored_size != sizeof(diagnostics_persistent_state_v1_t)) {
        return false;
    }
    diagnostics_persistent_state_v1_t legacy = {0};
    result = nvs_get_blob(
        diagnostics_nvs_handle,
        DIAGNOSTICS_NVS_KEY,
        &legacy,
        &stored_size);
    if (result != ESP_OK || !persistent_state_header_valid(
            legacy.magic,
            legacy.version,
            legacy.event_head,
            legacy.event_count,
            DIAGNOSTICS_PERSISTENT_VERSION_LEGACY_32_BIT_TIME)) {
        return false;
    }

    memset(&persistent, 0, sizeof(persistent));
    persistent.magic = legacy.magic;
    persistent.version = DIAGNOSTICS_PERSISTENT_VERSION;
    persistent.boot_count = legacy.boot_count;
    persistent.next_sequence = legacy.next_sequence;
    persistent.event_head = legacy.event_head;
    persistent.event_count = legacy.event_count;
    persistent.last_ota_result = legacy.last_ota_result;
    for (size_t index = 0; index < DIAGNOSTICS_FAULT_LOG_CAPACITY; ++index) {
        persistent.events[index] = (diagnostics_fault_event_t){
            .sequence = legacy.events[index].sequence,
            .boot_count = legacy.events[index].boot_count,
            .uptime_ms = legacy.events[index].uptime_ms,
            .type = legacy.events[index].type,
            .code = legacy.events[index].code,
        };
    }
    ESP_LOGI(TAG, "migrated persistent diagnostics to 64-bit timestamps");
    return true;
}

static void record_event_locked(diagnostics_event_type_t type, int code)
{
    diagnostics_fault_event_t *event =
        &persistent.events[persistent.event_head];
    *event = (diagnostics_fault_event_t){
        .sequence = persistent.next_sequence++,
        .boot_count = persistent.boot_count,
        .uptime_ms = uptime_ms(),
        .type = (uint16_t)type,
        .code = (int16_t)code,
    };
    persistent.event_head =
        (persistent.event_head + 1U) % DIAGNOSTICS_FAULT_LOG_CAPACITY;
    if (persistent.event_count < DIAGNOSTICS_FAULT_LOG_CAPACITY) {
        persistent.event_count++;
    }
    save_persistent_locked();
}

static esp_err_t initialize_temperature_sensor(void)
{
    const temperature_sensor_config_t config =
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    esp_err_t result = temperature_sensor_install(&config, &temperature_sensor);
    if (result == ESP_OK) {
        result = temperature_sensor_enable(temperature_sensor);
    }
    if (result != ESP_OK) {
        temperature_sensor = NULL;
        diagnostics_record_event(
            DIAGNOSTICS_EVENT_TEMPERATURE_SENSOR_FAILED, result);
    }
    return result;
}

esp_err_t diagnostics_init(void)
{
    state_mutex = xSemaphoreCreateMutex();
    if (state_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
        result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "NVS erase failed");
        result = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(result, TAG, "NVS initialization failed");
    ESP_RETURN_ON_ERROR(
        nvs_open(DIAGNOSTICS_NVS_NAMESPACE, NVS_READWRITE, &diagnostics_nvs_handle),
        TAG,
        "diagnostics NVS open failed");
    nvs_ready = true;

    if (!load_persistent_state()) {
        memset(&persistent, 0, sizeof(persistent));
        persistent.magic = DIAGNOSTICS_PERSISTENT_MAGIC;
        persistent.version = DIAGNOSTICS_PERSISTENT_VERSION;
        persistent.next_sequence = 1U;
    }

    startup_reset_reason = esp_reset_reason();
    persistent.boot_count++;
    record_event_locked(DIAGNOSTICS_EVENT_BOOT, startup_reset_reason);
    if (abnormal_reset(startup_reset_reason)) {
        record_event_locked(
            DIAGNOSTICS_EVENT_ABNORMAL_RESET, startup_reset_reason);
    }

    memset(&network_state, 0, sizeof(network_state));
    last_acquired_ipv4_address = 0U;
    for (size_t index = 0; index < DIAGNOSTICS_BACNET_COUNTER_COUNT; ++index) {
        atomic_init(&bacnet_counters[index], 0U);
    }
    atomic_init(&active_cov_subscriptions, 0U);
    atomic_init(&temperature_read_failure_recorded, false);
    for (size_t index = 0; index < DIAGNOSTICS_TASK_COUNT; ++index) {
        task_last_heartbeat[index] = 0U;
        atomic_init(&task_watchdog_subscribed[index], false);
    }

    const esp_err_t temperature_result = initialize_temperature_sensor();
    if (temperature_result != ESP_OK) {
        ESP_LOGW(TAG, "chip temperature unavailable");
    }
    ESP_LOGI(
        TAG,
        "boot %u, reset reason %s, firmware commit %s",
        (unsigned)persistent.boot_count,
        diagnostics_reset_reason_name(startup_reset_reason),
        PROJECT_GIT_REVISION);
    return ESP_OK;
}

const char *diagnostics_git_revision(void)
{
    return PROJECT_GIT_REVISION;
}

const char *diagnostics_reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "power-on";
    case ESP_RST_EXT:
        return "external";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt-watchdog";
    case ESP_RST_TASK_WDT:
        return "task-watchdog";
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deep-sleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    case ESP_RST_USB:
        return "usb";
    case ESP_RST_JTAG:
        return "jtag";
    case ESP_RST_EFUSE:
        return "efuse";
    case ESP_RST_PWR_GLITCH:
        return "power-glitch";
    case ESP_RST_CPU_LOCKUP:
        return "cpu-lockup";
    case ESP_RST_UNKNOWN:
    default:
        return "unknown";
    }
}

const char *diagnostics_event_name(diagnostics_event_type_t event)
{
    switch (event) {
    case DIAGNOSTICS_EVENT_BOOT:
        return "boot";
    case DIAGNOSTICS_EVENT_ABNORMAL_RESET:
        return "abnormal-reset";
    case DIAGNOSTICS_EVENT_ETHERNET_LINK_LOST:
        return "ethernet-link-lost";
    case DIAGNOSTICS_EVENT_IP_LOST:
        return "ip-lost";
    case DIAGNOSTICS_EVENT_OTA_ACCEPTED:
        return "ota-accepted";
    case DIAGNOSTICS_EVENT_OTA_FAILED:
        return "ota-failed";
    case DIAGNOSTICS_EVENT_OTA_VALIDATED:
        return "ota-validated";
    case DIAGNOSTICS_EVENT_INPUT_SELF_TEST_FAILED:
        return "input-self-test-failed";
    case DIAGNOSTICS_EVENT_TEMPERATURE_SENSOR_FAILED:
        return "temperature-sensor-failed";
    case DIAGNOSTICS_EVENT_BACNET_SOCKET_FAILED:
        return "bacnet-socket-failed";
    case DIAGNOSTICS_EVENT_OTA_SERVER_FAILED:
        return "ota-server-failed";
    case DIAGNOSTICS_EVENT_TASK_WATCHDOG_FAILED:
        return "task-watchdog-failed";
    case DIAGNOSTICS_EVENT_REMOTE_REBOOT_REQUESTED:
        return "remote-reboot-requested";
    default:
        return "unknown";
    }
}

const char *diagnostics_ota_result_name(diagnostics_ota_result_t result)
{
    switch (result) {
    case DIAGNOSTICS_OTA_ACCEPTED:
        return "accepted";
    case DIAGNOSTICS_OTA_FAILED:
        return "failed";
    case DIAGNOSTICS_OTA_VALIDATED:
        return "validated";
    case DIAGNOSTICS_OTA_NONE:
    default:
        return "none";
    }
}

void diagnostics_record_event(diagnostics_event_type_t type, int code)
{
    if (state_mutex == NULL ||
        xSemaphoreTake(state_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    record_event_locked(type, code);
    xSemaphoreGive(state_mutex);
}

void diagnostics_record_ota_result(diagnostics_ota_result_t result, int code)
{
    if (state_mutex == NULL ||
        xSemaphoreTake(state_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    persistent.last_ota_result = (uint32_t)result;
    diagnostics_event_type_t event = DIAGNOSTICS_EVENT_OTA_FAILED;
    if (result == DIAGNOSTICS_OTA_ACCEPTED) {
        event = DIAGNOSTICS_EVENT_OTA_ACCEPTED;
    } else if (result == DIAGNOSTICS_OTA_VALIDATED) {
        event = DIAGNOSTICS_EVENT_OTA_VALIDATED;
    }
    record_event_locked(event, code);
    xSemaphoreGive(state_mutex);
}

void diagnostics_set_ethernet_handle(esp_eth_handle_t handle)
{
    ethernet_handle = handle;
}

void diagnostics_ethernet_link_changed(bool link_up)
{
    if (state_mutex == NULL ||
        xSemaphoreTake(state_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    network_state.link_up = link_up;
    if (link_up) {
        network_state.link_up_count++;
        network_state.reconnect_count =
            network_state.link_up_count > 0U
            ? network_state.link_up_count - 1U
            : 0U;
        if (ethernet_handle != NULL) {
            eth_speed_t speed = ETH_SPEED_MAX;
            eth_duplex_t duplex = ETH_DUPLEX_HALF;
            bool autonegotiation = false;
            uint8_t mac[6] = {0};
            if (esp_eth_ioctl(
                    ethernet_handle, ETH_CMD_G_SPEED, &speed) == ESP_OK) {
                network_state.speed_mbps =
                    speed == ETH_SPEED_100M ? 100U : 10U;
            }
            if (esp_eth_ioctl(
                    ethernet_handle,
                    ETH_CMD_G_DUPLEX_MODE,
                    &duplex) == ESP_OK) {
                network_state.full_duplex = duplex == ETH_DUPLEX_FULL;
            }
            if (esp_eth_ioctl(
                    ethernet_handle,
                    ETH_CMD_G_AUTONEGO,
                    &autonegotiation) == ESP_OK) {
                network_state.autonegotiation = autonegotiation;
            }
            if (esp_eth_ioctl(
                    ethernet_handle, ETH_CMD_G_MAC_ADDR, mac) == ESP_OK) {
                memcpy(network_state.mac, mac, sizeof(mac));
            }
        }
    } else {
        network_state.link_down_count++;
        network_state.ipv4_address = 0U;
        network_state.ipv4_netmask = 0U;
        network_state.ipv4_gateway = 0U;
        memset(network_state.ipv4_dns, 0, sizeof(network_state.ipv4_dns));
        diagnostics_event_type_t event =
            DIAGNOSTICS_EVENT_ETHERNET_LINK_LOST;
        xSemaphoreGive(state_mutex);
        diagnostics_record_event(event, 0);
        return;
    }
    xSemaphoreGive(state_mutex);
}

void diagnostics_ip_acquired(
    esp_netif_t *netif,
    const esp_netif_ip_info_t *ip_info)
{
    if (netif == NULL || ip_info == NULL || state_mutex == NULL ||
        xSemaphoreTake(state_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (last_acquired_ipv4_address != 0U &&
        last_acquired_ipv4_address != ip_info->ip.addr) {
        network_state.ip_changed_count++;
    }
    last_acquired_ipv4_address = ip_info->ip.addr;
    network_state.ipv4_address = ip_info->ip.addr;
    network_state.ipv4_netmask = ip_info->netmask.addr;
    network_state.ipv4_gateway = ip_info->gw.addr;
    for (size_t index = 0U;
         index < sizeof(network_state.ipv4_dns) /
             sizeof(network_state.ipv4_dns[0]);
         ++index) {
        esp_netif_dns_info_t dns = {0};
        if (esp_netif_get_dns_info(
                netif, (esp_netif_dns_type_t)index, &dns) == ESP_OK &&
            dns.ip.type == ESP_IPADDR_TYPE_V4) {
            network_state.ipv4_dns[index] = dns.ip.u_addr.ip4.addr;
        } else {
            network_state.ipv4_dns[index] = 0U;
        }
    }
    network_state.ip_acquisition_count++;
    network_state.ip_acquired_uptime_ms = uptime_ms();
    esp_netif_dhcp_status_t status = ESP_NETIF_DHCP_INIT;
    if (esp_netif_dhcpc_get_status(netif, &status) == ESP_OK) {
        network_state.dhcp_status = (uint32_t)status;
    }
    xSemaphoreGive(state_mutex);
}

void diagnostics_ip_lost(void)
{
    if (state_mutex != NULL &&
        xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
        network_state.ipv4_address = 0U;
        network_state.ipv4_netmask = 0U;
        network_state.ipv4_gateway = 0U;
        memset(network_state.ipv4_dns, 0, sizeof(network_state.ipv4_dns));
        xSemaphoreGive(state_mutex);
    }
    diagnostics_record_event(DIAGNOSTICS_EVENT_IP_LOST, 0);
}

void diagnostics_bacnet_increment(diagnostics_bacnet_counter_t counter)
{
    if ((size_t)counter < DIAGNOSTICS_BACNET_COUNTER_COUNT) {
        atomic_fetch_add_explicit(
            &bacnet_counters[counter], 1U, memory_order_relaxed);
    }
}

void diagnostics_bacnet_set_active_subscriptions(uint32_t count)
{
    atomic_store_explicit(
        &active_cov_subscriptions, count, memory_order_relaxed);
}

esp_err_t diagnostics_task_watchdog_subscribe(diagnostics_task_t task)
{
    if ((size_t)task >= DIAGNOSTICS_TASK_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t result = esp_task_wdt_add(NULL);
    if (result == ESP_OK) {
        atomic_store_explicit(
            &task_watchdog_subscribed[task], true, memory_order_release);
        diagnostics_task_heartbeat(task);
    } else {
        diagnostics_record_event(
            DIAGNOSTICS_EVENT_TASK_WATCHDOG_FAILED, result);
    }
    return result;
}

void diagnostics_task_heartbeat(diagnostics_task_t task)
{
    if ((size_t)task >= DIAGNOSTICS_TASK_COUNT) {
        return;
    }
    const uint64_t heartbeat_ms = uptime_ms();
    portENTER_CRITICAL(&task_heartbeat_lock);
    task_last_heartbeat[task] = heartbeat_ms;
    portEXIT_CRITICAL(&task_heartbeat_lock);
    if (atomic_load_explicit(
            &task_watchdog_subscribed[task], memory_order_acquire)) {
        (void)esp_task_wdt_reset();
    }
}

bool diagnostics_snapshot_get(diagnostics_snapshot_t *snapshot)
{
    if (snapshot == NULL || state_mutex == NULL) {
        return false;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->uptime_ms = uptime_ms();
    snapshot->free_heap_bytes = esp_get_free_heap_size();
    snapshot->minimum_free_heap_bytes = esp_get_minimum_free_heap_size();
    snapshot->reset_reason = startup_reset_reason;
    if (temperature_sensor != NULL) {
        const esp_err_t temperature_result = temperature_sensor_get_celsius(
            temperature_sensor, &snapshot->chip_temperature_c);
        if (temperature_result == ESP_OK) {
            snapshot->chip_temperature_valid = true;
        } else if (!atomic_exchange_explicit(
                       &temperature_read_failure_recorded,
                       true,
                       memory_order_acq_rel)) {
            diagnostics_record_event(
                DIAGNOSTICS_EVENT_TEMPERATURE_SENSOR_FAILED,
                temperature_result);
        }
    }

    if (xSemaphoreTake(state_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    snapshot->boot_count = persistent.boot_count;
    snapshot->last_ota_result =
        (diagnostics_ota_result_t)persistent.last_ota_result;
    snapshot->network = network_state;
    snapshot->fault_log_count = persistent.event_count;
    const uint32_t oldest =
        (persistent.event_head + DIAGNOSTICS_FAULT_LOG_CAPACITY -
         persistent.event_count) %
        DIAGNOSTICS_FAULT_LOG_CAPACITY;
    for (size_t index = 0; index < persistent.event_count; ++index) {
        snapshot->fault_log[index] = persistent.events[
            (oldest + index) % DIAGNOSTICS_FAULT_LOG_CAPACITY];
    }
    xSemaphoreGive(state_mutex);

    for (size_t index = 0; index < DIAGNOSTICS_BACNET_COUNTER_COUNT; ++index) {
        snapshot->bacnet[index] = (uint32_t)atomic_load_explicit(
            &bacnet_counters[index], memory_order_relaxed);
    }
    snapshot->active_cov_subscriptions = (uint32_t)atomic_load_explicit(
        &active_cov_subscriptions, memory_order_relaxed);
    portENTER_CRITICAL(&task_heartbeat_lock);
    for (size_t index = 0; index < DIAGNOSTICS_TASK_COUNT; ++index) {
        snapshot->task_last_heartbeat_ms[index] = task_last_heartbeat[index];
    }
    portEXIT_CRITICAL(&task_heartbeat_lock);
    for (size_t index = 0; index < DIAGNOSTICS_TASK_COUNT; ++index) {
        snapshot->task_watchdog_subscribed[index] = atomic_load_explicit(
            &task_watchdog_subscribed[index], memory_order_acquire);
        snapshot->task_healthy[index] = diagnostics_heartbeat_is_healthy(
            snapshot->task_watchdog_subscribed[index],
            snapshot->uptime_ms,
            snapshot->task_last_heartbeat_ms[index],
            DIAGNOSTICS_TASK_HEALTH_MAX_AGE_MS);
    }
    return true;
}
