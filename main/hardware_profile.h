#pragma once

#include <stdbool.h>

#define HARDWARE_PROFILE_BOARD_MODEL "Waveshare ESP32-P4-POE-ETH"
#define HARDWARE_PROFILE_HEADER_NAME "P1"
#define HARDWARE_PROFILE_3V3_POSITION 36
#define HARDWARE_PROFILE_EXTERNAL_PULL_DOWN_OHMS 10000

int hardware_profile_p1_position(int gpio);
const char *hardware_profile_binary_state(bool raw_level, bool active_low);
