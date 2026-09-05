#include "diagnostics_log_model.h"

#include <string.h>
#include "clock_model.h"

typedef struct {
    uint32_t sequence, boot_count, uptime_ms;
    uint16_t type;
    int16_t code;
} event_v1_t;

typedef struct {
    uint32_t magic, version, boot_count, next_sequence, event_head, event_count, last_ota_result;
    event_v1_t events[DIAGNOSTICS_FAULT_LOG_CAPACITY];
} state_v1_t;

_Static_assert(sizeof(state_v1_t) == 284U, "v1 log storage layout changed");
_Static_assert(sizeof(diagnostics_persistent_state_v2_t) == 416U, "v2 log storage layout changed");
_Static_assert(sizeof(diagnostics_persistent_state_t) == 544U, "v3 log storage layout changed");

bool diagnostics_log_decode(const void *data, size_t size, diagnostics_persistent_state_t *state)
{
    uint32_t header[7];
    if (data == NULL || state == NULL || size < sizeof(header)) return false;
    memcpy(header, data, sizeof(header));
    if (header[0] != DIAGNOSTICS_PERSISTENT_MAGIC || header[3] == 0U ||
        header[4] >= DIAGNOSTICS_FAULT_LOG_CAPACITY ||
        header[5] > DIAGNOSTICS_FAULT_LOG_CAPACITY) return false;
    if (header[1] == 3U && size == sizeof(*state)) {
        memcpy(state, data, sizeof(*state));
        for (uint32_t index = 0U; index < state->event_count; ++index) {
            const diagnostics_fault_event_t *event = &state->events[
                (state->event_head + DIAGNOSTICS_FAULT_LOG_CAPACITY - state->event_count + index)
                % DIAGNOSTICS_FAULT_LOG_CAPACITY];
            if (event->clock_quality > CLOCK_STALE ||
                (event->clock_quality == CLOCK_UNSYNCHRONIZED && event->utc_unix_ms != 0U) ||
                (event->clock_quality != CLOCK_UNSYNCHRONIZED &&
                 (event->utc_unix_ms < CLOCK_MIN_UNIX_MS || event->utc_unix_ms >= CLOCK_MAX_UNIX_MS))) {
                return false;
            }
        }
        return true;
    }
    if (!((header[1] == 1U && size == sizeof(state_v1_t)) ||
          (header[1] == 2U && size == sizeof(diagnostics_persistent_state_v2_t)))) return false;
    memset(state, 0, sizeof(*state));
    memcpy(state, header, sizeof(header));
    state->version = DIAGNOSTICS_PERSISTENT_VERSION;
    for (size_t index = 0U; index < DIAGNOSTICS_FAULT_LOG_CAPACITY; ++index) {
        diagnostics_fault_event_t *event = &state->events[index];
        if (header[1] == 1U) {
            event_v1_t old;
            memcpy(&old, (const uint8_t *)data + offsetof(state_v1_t, events) + index * sizeof(old), sizeof(old));
            event->sequence = old.sequence;
            event->boot_count = old.boot_count;
            event->uptime_ms = old.uptime_ms;
            event->type = old.type;
            event->code = old.code;
        } else {
            diagnostics_fault_event_v2_t old;
            memcpy(&old, (const uint8_t *)data + offsetof(diagnostics_persistent_state_v2_t, events) + index * sizeof(old), sizeof(old));
            event->sequence = old.sequence;
            event->boot_count = old.boot_count;
            event->uptime_ms = old.uptime_ms;
            event->type = old.type;
            event->code = old.code;
        }
    }
    return true;
}

void diagnostics_log_to_v2(const diagnostics_persistent_state_t *state, diagnostics_persistent_state_v2_t *legacy)
{
    if (state == NULL || legacy == NULL) return;
    memset(legacy, 0, sizeof(*legacy));
    memcpy(legacy, state, 7U * sizeof(uint32_t));
    legacy->version = 2U;
    for (size_t index = 0U; index < DIAGNOSTICS_FAULT_LOG_CAPACITY; ++index) {
        legacy->events[index] = (diagnostics_fault_event_v2_t){
            .sequence = state->events[index].sequence,
            .boot_count = state->events[index].boot_count,
            .uptime_ms = state->events[index].uptime_ms,
            .type = state->events[index].type,
            .code = state->events[index].code,
        };
    }
}

void diagnostics_log_reconcile(diagnostics_persistent_state_t *state, const diagnostics_persistent_state_t *legacy)
{
    if (state == NULL || legacy == NULL ||
        legacy->boot_count < state->boot_count ||
        (legacy->boot_count == state->boot_count && legacy->next_sequence <= state->next_sequence)) return;
    diagnostics_persistent_state_t updated = *legacy;
    for (size_t index = 0U; index < DIAGNOSTICS_FAULT_LOG_CAPACITY; ++index) {
        diagnostics_fault_event_t *event = &updated.events[index];
        for (size_t old_index = 0U; old_index < DIAGNOSTICS_FAULT_LOG_CAPACITY; ++old_index) {
            const diagnostics_fault_event_t *old = &state->events[old_index];
            if (event->sequence != 0U && event->sequence == old->sequence &&
                event->boot_count == old->boot_count && event->uptime_ms == old->uptime_ms &&
                event->type == old->type && event->code == old->code) {
                event->utc_unix_ms = old->utc_unix_ms;
                event->clock_quality = old->clock_quality;
                break;
            }
        }
    }
    *state = updated;
}
