#pragma once

#include <stdbool.h>
#include <stddef.h>

#define HARDWARE_PROFILE_BOARD_MODEL "Waveshare ESP32-P4-POE-ETH"
#define HARDWARE_PROFILE_HEADER_NAME "P1"
#define HARDWARE_PROFILE_3V3_POSITION 36
#define HARDWARE_PROFILE_EXTERNAL_PULL_DOWN_OHMS 10000
#define HARDWARE_PROFILE_P1_COUNT 40U

typedef struct {
    int gpio; /* -1 for power, ground, and control nets. */
    const char *label;
    const char *kind;
    const char *usage;
    bool diagnostic_input;
} hardware_profile_pin_t;

/* Zero-based array index; physical P1 position is index + 1. */
const hardware_profile_pin_t *hardware_profile_p1_pin(size_t index);

int hardware_profile_p1_position(int gpio);
const char *hardware_profile_binary_state(bool raw_level, bool active_low);
