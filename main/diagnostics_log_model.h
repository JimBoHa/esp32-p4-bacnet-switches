#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DIAGNOSTICS_FAULT_LOG_CAPACITY 16U
#define DIAGNOSTICS_PERSISTENT_MAGIC 0x44494147U
#define DIAGNOSTICS_PERSISTENT_VERSION 3U

typedef struct {
    uint32_t sequence;
    uint32_t boot_count;
    uint64_t uptime_ms;
    uint64_t utc_unix_ms;
    uint16_t type;
    int16_t code;
    uint8_t clock_quality;
} diagnostics_fault_event_t;

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

/* Kept as a compatibility mirror so rollback firmware can retain its log. */
typedef struct {
    uint32_t sequence;
    uint32_t boot_count;
    uint64_t uptime_ms;
    uint16_t type;
    int16_t code;
} diagnostics_fault_event_v2_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t boot_count;
    uint32_t next_sequence;
    uint32_t event_head;
    uint32_t event_count;
    uint32_t last_ota_result;
    diagnostics_fault_event_v2_t events[DIAGNOSTICS_FAULT_LOG_CAPACITY];
} diagnostics_persistent_state_v2_t;

bool diagnostics_log_decode(const void *data, size_t size, diagnostics_persistent_state_t *state);
void diagnostics_log_to_v2(const diagnostics_persistent_state_t *state, diagnostics_persistent_state_v2_t *legacy);
void diagnostics_log_reconcile(diagnostics_persistent_state_t *state, const diagnostics_persistent_state_t *legacy);
