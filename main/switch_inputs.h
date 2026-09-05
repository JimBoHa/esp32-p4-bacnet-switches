#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "config_model.h"
#include "input_line_classifier.h"

#define SWITCH_INPUT_COUNT 3U

typedef struct {
    bool valid;
    uint32_t function_select;
    uint32_t output_signal;
    uint32_t drive_capability;
    bool pull_up_enabled;
    bool pull_down_enabled;
    bool input_enabled;
    bool output_enabled;
    bool output_enable_controlled_by_peripheral;
    bool output_enable_inverted;
    bool open_drain_enabled;
    bool sleep_select_enabled;
} switch_input_pad_config_t;

typedef struct {
    int gpio;
    bool active_low;
    uint32_t debounce_ms;
    switch_input_pad_config_t startup_config;
    bool startup_raw_after_input_enable;
    switch_input_pad_config_t configured_config;
    bool configured_raw;
    switch_input_pad_config_t current_config;
    bool current_raw;
    bool stable;
    uint32_t transition_count;
    uint64_t last_transition_uptime_ms;
    uint32_t raw_edge_count;
    uint32_t accepted_transition_count;
    uint32_t rejected_pulse_count;
    uint32_t chatter_event_count;
    bool chattering;
    bool candidate_active;
    bool candidate_level;
    uint32_t candidate_age_ms;
    uint32_t last_rejected_pulse_width_ms;
    uint64_t last_raw_edge_uptime_ms;
    uint64_t last_rejected_pulse_uptime_ms;
    bool self_test_run;
    bool self_test_passed;
    bool self_test_pull_down_level;
    bool self_test_pull_up_level;
    bool self_test_pull_down_stable;
    bool self_test_pull_up_stable;
    input_line_classification_t self_test_classification;
} switch_input_diagnostics_t;

void switch_inputs_init(const firmware_config_t *config);
bool switch_input_get(size_t index);
bool switch_input_faulted(size_t index);
bool switch_input_active_low(size_t index);
esp_err_t switch_inputs_run_self_test(void);
bool switch_input_diagnostics_get(
    size_t index,
    switch_input_diagnostics_t *diagnostics);
