#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SWITCH_INPUT_COUNT 2U

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
    switch_input_pad_config_t startup_config;
    bool startup_raw_after_input_enable;
    switch_input_pad_config_t configured_config;
    bool configured_raw;
    switch_input_pad_config_t current_config;
    bool current_raw;
    bool stable;
} switch_input_diagnostics_t;

void switch_inputs_init(void);
bool switch_input_get(size_t index);
bool switch_input_diagnostics_get(
    size_t index,
    switch_input_diagnostics_t *diagnostics);
