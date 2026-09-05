#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INPUT_HISTORY_CAPACITY 64U

typedef enum {
    INPUT_HISTORY_INITIAL = 1,
    INPUT_HISTORY_TRANSITION,
    INPUT_HISTORY_REJECTED_PULSE,
    INPUT_HISTORY_CHATTER_STARTED,
} input_history_kind_t;

typedef struct {
    uint64_t sequence;
    uint64_t uptime_ms;
    uint32_t pulse_width_ms;
    uint16_t gpio;
    uint8_t kind;
    bool active;
} input_history_event_t;

/* Callers synchronize access. Events are retained oldest-to-newest in RAM. */
typedef struct {
    input_history_event_t events[INPUT_HISTORY_CAPACITY];
    uint64_t total_events;
    size_t head;
    size_t count;
} input_history_t;

void input_history_init(input_history_t *history);
bool input_history_record(
    input_history_t *history,
    int gpio,
    input_history_kind_t kind,
    bool active,
    uint32_t pulse_width_ms,
    uint64_t uptime_ms);
bool input_history_get(
    const input_history_t *history,
    size_t oldest_index,
    input_history_event_t *event);
const char *input_history_kind_name(input_history_kind_t kind);
