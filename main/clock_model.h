#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLOCK_STALE_AFTER_MS (2ULL * 60U * 60U * 1000U)
#define CLOCK_MIN_UNIX_MS 1577836800000ULL /* 2020-01-01 UTC */
#define CLOCK_MAX_UNIX_MS 4102444800000ULL /* 2100-01-01 UTC */
#define CLOCK_SERVER_MAX_LENGTH 253U

typedef enum {
    CLOCK_UNSYNCHRONIZED = 0,
    CLOCK_SYNCHRONIZED = 1,
    CLOCK_STALE = 2,
} clock_quality_t;

typedef struct {
    uint64_t uptime_ms;
    uint64_t unix_ms; /* zero means unknown, never a fabricated epoch date */
    clock_quality_t quality;
} clock_stamp_t;

typedef struct {
    uint64_t last_sync_uptime_ms;
    uint64_t last_sync_unix_ms;
    uint32_t sync_count;
    uint32_t rejected_sync_count;
} clock_model_t;

bool clock_model_sync(clock_model_t *clock, uint64_t uptime_ms, uint64_t unix_ms);
clock_stamp_t clock_model_stamp(const clock_model_t *clock, uint64_t uptime_ms);
const char *clock_quality_name(clock_quality_t quality);
bool clock_server_name_valid(const char *server);
