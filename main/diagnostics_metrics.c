#include "diagnostics_metrics.h"

#include <limits.h>

static uint32_t saturating_increment(uint32_t value)
{
    return value == UINT32_MAX ? UINT32_MAX : value + 1U;
}

void diagnostics_temperature_metrics_record(
    diagnostics_temperature_metrics_t *metrics,
    bool success,
    float sample_c,
    int32_t result)
{
    if (metrics == NULL) {
        return;
    }
    metrics->last_result = result;
    if (!success) {
        metrics->error_count = saturating_increment(metrics->error_count);
        return;
    }
    if (!metrics->has_sample) {
        metrics->minimum_c = sample_c;
        metrics->maximum_c = sample_c;
        metrics->has_sample = true;
    } else {
        if (sample_c < metrics->minimum_c) {
            metrics->minimum_c = sample_c;
        }
        if (sample_c > metrics->maximum_c) {
            metrics->maximum_c = sample_c;
        }
    }
    metrics->sample_count = saturating_increment(metrics->sample_count);
}

diagnostics_fault_log_metrics_t diagnostics_fault_log_metrics(
    uint32_t next_sequence,
    size_t retained_count)
{
    const uint32_t total = next_sequence == 0U ? UINT32_MAX : next_sequence - 1U;
    const uint32_t retained = retained_count > UINT32_MAX
        ? UINT32_MAX
        : (uint32_t)retained_count;
    return (diagnostics_fault_log_metrics_t){
        .total_event_count = total,
        .overwritten_event_count = total > retained ? total - retained : 0U,
    };
}
