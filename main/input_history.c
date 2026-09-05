#include "input_history.h"

#include <limits.h>

void input_history_init(input_history_t *history)
{
    if (history != NULL) {
        *history = (input_history_t){0};
    }
}

bool input_history_record(
    input_history_t *history,
    int gpio,
    input_history_kind_t kind,
    bool active,
    uint32_t pulse_width_ms,
    clock_stamp_t stamp)
{
    if (history == NULL || gpio < 0 || gpio > 54 ||
        kind < INPUT_HISTORY_INITIAL || kind > INPUT_HISTORY_CHATTER_STARTED ||
        history->head >= INPUT_HISTORY_CAPACITY ||
        history->count > INPUT_HISTORY_CAPACITY ||
        history->total_events == UINT64_MAX ||
        stamp.quality < CLOCK_UNSYNCHRONIZED || stamp.quality > CLOCK_STALE ||
        (stamp.quality == CLOCK_UNSYNCHRONIZED && stamp.unix_ms != 0U) ||
        (stamp.quality != CLOCK_UNSYNCHRONIZED &&
         (stamp.unix_ms < CLOCK_MIN_UNIX_MS || stamp.unix_ms >= CLOCK_MAX_UNIX_MS))) {
        return false;
    }
    history->total_events++;
    history->events[history->head] = (input_history_event_t){
        .sequence = history->total_events,
        .uptime_ms = stamp.uptime_ms,
        .utc_unix_ms = stamp.unix_ms,
        .clock_quality = (uint8_t)stamp.quality,
        .pulse_width_ms = kind == INPUT_HISTORY_REJECTED_PULSE
            ? pulse_width_ms : 0U,
        .gpio = (uint16_t)gpio,
        .kind = (uint8_t)kind,
        .active = active,
    };
    history->head = (history->head + 1U) % INPUT_HISTORY_CAPACITY;
    if (history->count < INPUT_HISTORY_CAPACITY) {
        history->count++;
    }
    return true;
}

bool input_history_get(
    const input_history_t *history,
    size_t oldest_index,
    input_history_event_t *event)
{
    if (history == NULL || event == NULL ||
        history->head >= INPUT_HISTORY_CAPACITY ||
        history->count > INPUT_HISTORY_CAPACITY || oldest_index >= history->count) {
        return false;
    }
    const size_t oldest =
        (history->head + INPUT_HISTORY_CAPACITY - history->count) %
        INPUT_HISTORY_CAPACITY;
    *event = history->events[(oldest + oldest_index) % INPUT_HISTORY_CAPACITY];
    return true;
}

const char *input_history_kind_name(input_history_kind_t kind)
{
    switch (kind) {
    case INPUT_HISTORY_INITIAL: return "initial-state";
    case INPUT_HISTORY_TRANSITION: return "state-changed";
    case INPUT_HISTORY_REJECTED_PULSE: return "rejected-pulse";
    case INPUT_HISTORY_CHATTER_STARTED: return "chatter-started";
    default: return "unknown";
    }
}
