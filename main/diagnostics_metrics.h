#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool has_sample;
    float minimum_c;
    float maximum_c;
    uint32_t sample_count;
    uint32_t error_count;
    int32_t last_result;
} diagnostics_temperature_metrics_t;

typedef struct {
    uint32_t total_event_count;
    uint32_t overwritten_event_count;
} diagnostics_fault_log_metrics_t;

void diagnostics_temperature_metrics_record(
    diagnostics_temperature_metrics_t *metrics,
    bool success,
    float sample_c,
    int32_t result);

diagnostics_fault_log_metrics_t diagnostics_fault_log_metrics(
    uint32_t next_sequence,
    size_t retained_count);
