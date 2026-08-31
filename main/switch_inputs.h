#pragma once

#include <stdbool.h>
#include <stddef.h>

#define SWITCH_INPUT_COUNT 2U

void switch_inputs_init(void);
bool switch_input_get(size_t index);
