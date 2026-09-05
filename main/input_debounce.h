#pragma once

#include <stdbool.h>
#include <stdint.h>

#define INPUT_CHATTER_REJECTION_THRESHOLD 4U
#define INPUT_CHATTER_WINDOW_MS 2000U
#define INPUT_CHATTER_HOLD_MS 5000U

typedef struct {
    bool initialized;
    bool stable;
    bool raw;
    bool candidate_active;
    bool candidate_level;
    uint32_t candidate_age_ms;
    uint32_t raw_edge_count;
    uint32_t accepted_transition_count;
    uint32_t rejected_pulse_count;
    uint32_t chatter_event_count;
    uint32_t rejection_window_count;
    uint8_t rejection_history_count;
    uint8_t rejection_history_next;
    uint32_t last_rejected_pulse_width_ms;
    uint64_t initial_observation_uptime_ms;
    uint64_t last_raw_edge_uptime_ms;
    uint64_t last_accepted_transition_uptime_ms;
    uint64_t last_rejected_pulse_uptime_ms;
    uint64_t chattering_until_uptime_ms;
    uint64_t rejection_uptime_history[INPUT_CHATTER_REJECTION_THRESHOLD];
} input_debounce_state_t;

typedef struct {
    bool raw_edge;
    bool accepted_transition;
    bool rejected_pulse;
    bool chatter_started;
    bool stable;
} input_debounce_result_t;

void input_debounce_init(
    input_debounce_state_t *state, bool initial_level, uint64_t uptime_ms);

/* Until the first accepted transition, age is measured from initial observation. */
uint64_t input_debounce_transition_age_ms(
    const input_debounce_state_t *state, uint64_t uptime_ms);

input_debounce_result_t input_debounce_sample(
    input_debounce_state_t *state,
    bool sampled_level,
    uint32_t sample_period_ms,
    uint32_t debounce_ms,
    uint64_t uptime_ms);

bool input_debounce_is_chattering(
    const input_debounce_state_t *state,
    uint64_t uptime_ms);
