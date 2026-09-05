#pragma once

#include <stdbool.h>
#include <stdint.h>

uint64_t diagnostics_milliseconds_from_microseconds(int64_t microseconds);

uint64_t diagnostics_elapsed_milliseconds(uint64_t now_ms, uint64_t since_ms);

bool diagnostics_heartbeat_is_healthy(
    bool subscribed,
    uint64_t now_ms,
    uint64_t last_heartbeat_ms,
    uint64_t maximum_age_ms);
