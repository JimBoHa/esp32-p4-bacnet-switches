#include "hardware_profile.h"

/* Waveshare ESP32-P4-ETH schematic, P1 "PICO-2*20 IO".
 * https://files.waveshare.com/wiki/ESP32-P4-ETH/ESP32-P4-ETH-datasheet.pdf
 * P1 positions are NOT ESP32 GPIO numbers or Raspberry Pi Pico GPIO numbers.
 * I2C net aliases are resolved at the ESP32-P4 symbol: SCL=8, SDA=7.
 */
static const hardware_profile_pin_t P1_PINS[HARDWARE_PROFILE_P1_COUNT] = {
    {54, "GPIO54", "gpio", "Header GPIO", true},
    {19, "GPIO19", "gpio", "Header GPIO", true},
    {-1, "GND", "ground", "Ground; not a digital input", false},
    {18, "GPIO18", "gpio", "Header GPIO", true},
    {17, "GPIO17", "gpio", "Header GPIO", true},
    {16, "GPIO16", "gpio", "Header GPIO", true},
    {15, "GPIO15", "gpio", "Header GPIO", true},
    {-1, "GND", "ground", "Ground; not a digital input", false},
    {14, "GPIO14", "gpio", "Header GPIO", true},
    {6, "GPIO6", "gpio", "Header GPIO", true},
    {5, "GPIO5", "gpio", "Header GPIO", true},
    {4, "GPIO4", "gpio", "Header GPIO", true},
    {-1, "GND", "ground", "Ground; not a digital input", false},
    {3, "GPIO3", "gpio", "Header GPIO", true},
    {2, "GPIO2", "gpio", "Header GPIO", true},
    {8, "ESP_I2C_SCL", "gpio", "Shared I2C clock (codec/display/camera)", true},
    {7, "ESP_I2C_SDA", "gpio", "Shared I2C data (codec/display/camera)", true},
    {-1, "GND", "ground", "Ground; not a digital input", false},
    {24, "USB1P1_N", "gpio", "USB D- / USB-JTAG; not reconfigured or sampled", false},
    {25, "USB1P1_P", "gpio", "USB D+ / USB-JTAG; not reconfigured or sampled", false},
    {48, "GPIO48", "gpio", "Header GPIO", true},
    {47, "GPIO47", "gpio", "Header GPIO", true},
    {-1, "GND", "ground", "Ground; not a digital input", false},
    {46, "GPIO46", "gpio", "Header GPIO", true},
    {33, "GPIO33", "gpio", "Header GPIO", true},
    {32, "GPIO32", "gpio", "Header GPIO", true},
    {27, "GPIO27", "gpio", "Header GPIO", true},
    {-1, "GND", "ground", "Ground; not a digital input", false},
    {26, "GPIO26", "gpio", "Header GPIO", true},
    {-1, "ESP_EN", "control", "Chip reset/enable; do not use as a switch input", false},
    {23, "GPIO23", "gpio", "Header GPIO", true},
    {22, "GPIO22", "gpio", "Header GPIO", true},
    {-1, "GND", "ground", "Ground; not a digital input", false},
    {21, "GPIO21", "gpio", "Header GPIO", true},
    {20, "GPIO20", "gpio", "Header GPIO", true},
    {-1, "ESP_3V3", "supply", "3.3 V supply; voltage is not measured", false},
    {-1, "3V3_EN", "control", "Regulator enable; do not use as a switch input", false},
    {-1, "GND", "ground", "Ground; not a digital input", false},
    {-1, "VCC_5V", "supply", "5 V supply; voltage is not measured", false},
    {-1, "VCC1_5V", "supply", "5 V supply; voltage is not measured", false},
};

const hardware_profile_pin_t *hardware_profile_p1_pin(size_t index)
{
    return index < HARDWARE_PROFILE_P1_COUNT ? &P1_PINS[index] : NULL;
}

int hardware_profile_p1_position(int gpio)
{
    if (gpio >= 0) {
        for (size_t index = 0; index < HARDWARE_PROFILE_P1_COUNT; ++index) {
            if (P1_PINS[index].gpio == gpio) {
                return (int)index + 1;
            }
        }
    }
    return -1;
}

const char *hardware_profile_binary_state(bool raw_level, bool active_low)
{
    return raw_level != active_low ? "active" : "inactive";
}
