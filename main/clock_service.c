#include "clock_service.h"

#include <stdatomic.h>
#include <sys/time.h>

#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

static clock_model_t model;
static portMUX_TYPE clock_lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool initialized;
static atomic_int_fast32_t last_error;

static void time_synchronized(struct timeval *time)
{
    const uint64_t uptime_ms = (uint64_t)esp_timer_get_time() / 1000U;
    uint64_t unix_ms = 0U;
    if (time != NULL && time->tv_sec >= 0 &&
        (uint64_t)time->tv_sec < CLOCK_MAX_UNIX_MS / 1000U &&
        time->tv_usec >= 0 && time->tv_usec < 1000000) {
        unix_ms = (uint64_t)time->tv_sec * 1000U + (uint64_t)time->tv_usec / 1000U;
    }
    /* The lwIP callback only updates bounded RAM; no logging, NVS, or blocking. */
    portENTER_CRITICAL(&clock_lock);
    (void)clock_model_sync(&model, uptime_ms, unix_ms);
    portEXIT_CRITICAL(&clock_lock);
}

const char *clock_service_configured_server(void)
{
    return clock_server_name_valid(CONFIG_DIAGNOSTICS_NTP_SERVER)
        ? CONFIG_DIAGNOSTICS_NTP_SERVER : "invalid";
}

esp_err_t clock_service_init(void)
{
    if (atomic_load(&initialized)) return ESP_OK;
    if (!clock_server_name_valid(CONFIG_DIAGNOSTICS_NTP_SERVER)) {
        atomic_store(&last_error, ESP_ERR_INVALID_ARG);
        return ESP_ERR_INVALID_ARG;
    }
    const bool dhcp = CONFIG_DIAGNOSTICS_NTP_SERVER[0] == '\0';
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_DIAGNOSTICS_NTP_SERVER);
    config.num_of_servers = dhcp ? 0U : 1U;
    config.server_from_dhcp = dhcp;
    config.start = false; /* start only after Ethernet obtains an address */
    config.wait_for_sync = false; /* time must never gate BACnet or OTA health */
    config.sync_cb = time_synchronized;
    const esp_err_t result = esp_netif_sntp_init(&config);
    atomic_store(&last_error, result);
    atomic_store(&initialized, result == ESP_OK);
    return result;
}

esp_err_t clock_service_start(void)
{
    const esp_err_t result = atomic_load(&initialized)
        ? esp_netif_sntp_start() : ESP_ERR_INVALID_STATE;
    atomic_store(&last_error, result);
    return result;
}

clock_stamp_t clock_service_stamp(uint64_t uptime_ms)
{
    portENTER_CRITICAL(&clock_lock);
    const clock_stamp_t stamp = clock_model_stamp(&model, uptime_ms);
    portEXIT_CRITICAL(&clock_lock);
    return stamp;
}

void clock_service_snapshot_get(clock_service_snapshot_t *snapshot)
{
    if (snapshot == NULL) return;
    portENTER_CRITICAL(&clock_lock);
    snapshot->clock = model;
    snapshot->now = clock_model_stamp(&model, (uint64_t)esp_timer_get_time() / 1000U);
    portEXIT_CRITICAL(&clock_lock);
    snapshot->initialized = atomic_load(&initialized);
    snapshot->last_error = (int32_t)atomic_load(&last_error);
}
