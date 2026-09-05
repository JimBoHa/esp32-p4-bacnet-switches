#include "input_debounce.h"

#include <limits.h>
#include <stddef.h>

static void increment_saturating(uint32_t *value)
{
    if (*value < UINT32_MAX) {
        (*value)++;
    }
}

static uint32_t add_saturating(uint32_t left, uint32_t right)
{
    return right > UINT32_MAX - left ? UINT32_MAX : left + right;
}

static uint64_t add_uptime_saturating(uint64_t left, uint64_t right)
{
    return right > UINT64_MAX - left ? UINT64_MAX : left + right;
}

void input_debounce_init(
    input_debounce_state_t *state, bool initial_level, uint64_t uptime_ms)
{
    if (state == NULL) {
        return;
    }
    *state = (input_debounce_state_t){
        .initialized = true,
        .stable = initial_level,
        .raw = initial_level,
        .candidate_level = initial_level,
        .initial_observation_uptime_ms = uptime_ms,
    };
}

uint64_t input_debounce_transition_age_ms(
    const input_debounce_state_t *state, uint64_t uptime_ms)
{
    if (state == NULL || !state->initialized) {
        return 0U;
    }
    const uint64_t since = state->accepted_transition_count != 0U
        ? state->last_accepted_transition_uptime_ms
        : state->initial_observation_uptime_ms;
    return uptime_ms >= since ? uptime_ms - since : 0U;
}

bool input_debounce_is_chattering(
    const input_debounce_state_t *state,
    uint64_t uptime_ms)
{
    return state != NULL &&
        state->chattering_until_uptime_ms > uptime_ms;
}

static uint32_t rejection_count_in_window(
    const input_debounce_state_t *state,
    uint64_t uptime_ms)
{
    uint32_t count = 0U;
    for (uint8_t index = 0U;
         index < state->rejection_history_count;
         ++index) {
        const uint64_t rejection_ms =
            state->rejection_uptime_history[index];
        if (uptime_ms >= rejection_ms &&
            uptime_ms - rejection_ms <= INPUT_CHATTER_WINDOW_MS) {
            count++;
        }
    }
    return count;
}

static void record_rejected_pulse(
    input_debounce_state_t *state,
    uint64_t uptime_ms,
    input_debounce_result_t *result)
{
    const bool was_chattering = input_debounce_is_chattering(
        state, uptime_ms);
    state->rejection_uptime_history[state->rejection_history_next] = uptime_ms;
    state->rejection_history_next = (uint8_t)(
        (state->rejection_history_next + 1U) %
        INPUT_CHATTER_REJECTION_THRESHOLD);
    if (state->rejection_history_count <
        INPUT_CHATTER_REJECTION_THRESHOLD) {
        state->rejection_history_count++;
    }
    state->rejection_window_count = rejection_count_in_window(
        state, uptime_ms);
    state->last_rejected_pulse_width_ms = state->candidate_age_ms;
    state->last_rejected_pulse_uptime_ms = uptime_ms;
    increment_saturating(&state->rejected_pulse_count);
    result->rejected_pulse = true;

    if (state->rejection_window_count >=
        INPUT_CHATTER_REJECTION_THRESHOLD) {
        if (!was_chattering) {
            increment_saturating(&state->chatter_event_count);
            result->chatter_started = true;
        }
        state->chattering_until_uptime_ms = add_uptime_saturating(
            uptime_ms, INPUT_CHATTER_HOLD_MS);
    }
}

input_debounce_result_t input_debounce_sample(
    input_debounce_state_t *state,
    bool sampled_level,
    uint32_t sample_period_ms,
    uint32_t debounce_ms,
    uint64_t uptime_ms)
{
    input_debounce_result_t result = {0};
    if (state == NULL) {
        return result;
    }
    if (!state->initialized) {
        input_debounce_init(state, sampled_level, uptime_ms);
        result.stable = sampled_level;
        return result;
    }

    if (sampled_level != state->raw) {
        state->raw = sampled_level;
        state->last_raw_edge_uptime_ms = uptime_ms;
        increment_saturating(&state->raw_edge_count);
        result.raw_edge = true;
    }

    if (sampled_level == state->stable) {
        if (state->candidate_active) {
            record_rejected_pulse(state, uptime_ms, &result);
        }
        state->candidate_active = false;
        state->candidate_level = state->stable;
        state->candidate_age_ms = 0U;
        result.stable = state->stable;
        return result;
    }

    if (!state->candidate_active ||
        state->candidate_level != sampled_level) {
        state->candidate_active = true;
        state->candidate_level = sampled_level;
        state->candidate_age_ms = sample_period_ms;
    } else {
        state->candidate_age_ms = add_saturating(
            state->candidate_age_ms, sample_period_ms);
    }

    if (state->candidate_age_ms >= debounce_ms) {
        state->stable = sampled_level;
        state->candidate_active = false;
        state->candidate_age_ms = 0U;
        state->last_accepted_transition_uptime_ms = uptime_ms;
        state->rejection_window_count = 0U;
        state->rejection_history_count = 0U;
        state->rejection_history_next = 0U;
        increment_saturating(&state->accepted_transition_count);
        result.accepted_transition = true;
    }
    result.stable = state->stable;
    return result;
}
