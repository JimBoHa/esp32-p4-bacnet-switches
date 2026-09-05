#include "hardware_profile.h"

int hardware_profile_p1_position(int gpio)
{
    switch (gpio) {
    case 20:
        return 35;
    case 21:
        return 34;
    case 22:
        return 32;
    default:
        return -1;
    }
}

const char *hardware_profile_binary_state(bool raw_level, bool active_low)
{
    return raw_level != active_low ? "active" : "inactive";
}
