#include "diagnostics_time.h"

uint64_t diagnostics_milliseconds_from_microseconds(int64_t microseconds)
{
    return microseconds > 0 ? (uint64_t)microseconds / 1000U : 0U;
}

uint64_t diagnostics_elapsed_milliseconds(uint64_t now_ms, uint64_t since_ms)
{
    return now_ms >= since_ms ? now_ms - since_ms : 0U;
}

bool diagnostics_heartbeat_is_healthy(
    bool subscribed,
    uint64_t now_ms,
    uint64_t last_heartbeat_ms,
    uint64_t maximum_age_ms)
{
    return subscribed && now_ms >= last_heartbeat_ms &&
        now_ms - last_heartbeat_ms <= maximum_age_ms;
}
