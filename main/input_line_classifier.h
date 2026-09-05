#pragma once

#include <stdbool.h>

typedef enum {
    INPUT_LINE_NOT_TESTED = 0,
    INPUT_LINE_FLOATING_OPEN,
    INPUT_LINE_EXTERNALLY_LOW,
    INPUT_LINE_EXTERNALLY_HIGH,
    INPUT_LINE_UNSTABLE,
} input_line_classification_t;

input_line_classification_t input_line_classify(
    bool pull_down_sample_stable,
    bool pull_down_level,
    bool pull_up_sample_stable,
    bool pull_up_level);

bool input_line_classification_valid(
    input_line_classification_t classification);

const char *input_line_classification_name(
    input_line_classification_t classification);
