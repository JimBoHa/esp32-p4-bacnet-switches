#include "input_line_classifier.h"

input_line_classification_t input_line_classify(
    bool pull_down_sample_stable,
    bool pull_down_level,
    bool pull_up_sample_stable,
    bool pull_up_level)
{
    if (!pull_down_sample_stable || !pull_up_sample_stable) {
        return INPUT_LINE_UNSTABLE;
    }
    if (!pull_down_level && pull_up_level) {
        return INPUT_LINE_FLOATING_OPEN;
    }
    if (!pull_down_level && !pull_up_level) {
        return INPUT_LINE_EXTERNALLY_LOW;
    }
    if (pull_down_level && pull_up_level) {
        return INPUT_LINE_EXTERNALLY_HIGH;
    }
    return INPUT_LINE_UNSTABLE;
}

bool input_line_classification_valid(
    input_line_classification_t classification)
{
    return classification == INPUT_LINE_FLOATING_OPEN ||
        classification == INPUT_LINE_EXTERNALLY_LOW ||
        classification == INPUT_LINE_EXTERNALLY_HIGH;
}

const char *input_line_classification_name(
    input_line_classification_t classification)
{
    switch (classification) {
    case INPUT_LINE_FLOATING_OPEN:
        return "floating-open";
    case INPUT_LINE_EXTERNALLY_LOW:
        return "externally-low";
    case INPUT_LINE_EXTERNALLY_HIGH:
        return "externally-high";
    case INPUT_LINE_UNSTABLE:
        return "unstable";
    case INPUT_LINE_NOT_TESTED:
    default:
        return "not-tested";
    }
}
