#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "header_diagnostics.h"
#include "hardware_profile.h"

static gpio_io_config_t pads[55];
static unsigned enables[55], reads[55];
static int scenario;
static int config_error_gpio = -1;

static void validate_gpio(int gpio)
{
    assert(gpio >= 0 && gpio < 55);
    const int position = hardware_profile_p1_position(gpio);
    assert(position > 0);
    assert(hardware_profile_p1_pin((size_t)position - 1U)->diagnostic_input);
}

esp_err_t gpio_get_io_config(int gpio, gpio_io_config_t *config)
{
    validate_gpio(gpio);
    if (gpio == config_error_gpio || (gpio == 2 && scenario == 1)) {
        return ESP_FAIL;
    }
    *config = pads[gpio];
    return ESP_OK;
}

esp_err_t gpio_input_enable(int gpio)
{
    validate_gpio(gpio);
    ++enables[gpio];
    assert(enables[gpio] == 1U);
    if (gpio == 2 && scenario == 2) {
        return ESP_FAIL;
    }
    if (!(gpio == 2 && scenario == 3)) {
        pads[gpio].ie = true;
    }
    if (gpio == 2 && scenario == 4) {
        pads[gpio].pu = !pads[gpio].pu;
    }
    return ESP_OK;
}

int gpio_get_level(int gpio)
{
    validate_gpio(gpio);
    assert(pads[gpio].ie);
    ++reads[gpio];
    return gpio == 47 || gpio == 54;
}

int64_t esp_timer_get_time(void)
{
    return INT64_C(5000000000000);
}

int main(int argc, char **argv)
{
    scenario = argc > 1 ? atoi(argv[1]) : 0;
    for (int gpio = 0; gpio < 55; ++gpio) {
        pads[gpio] = (gpio_io_config_t){
            .ie = gpio >= 20 && gpio <= 22,
            .pu = gpio % 2, .pd = gpio % 3 == 0, .oe = gpio % 4 == 0,
            .oe_ctrl_by_periph = gpio % 5 == 0, .oe_inv = gpio % 6 == 0,
            .od = gpio % 7 == 0, .slp_sel = gpio % 8 == 0,
            .drv = 3, .fun_sel = 1, .sig_out = 128U + (unsigned)gpio,
        };
    }
    gpio_io_config_t before[55];
    memcpy(before, pads, sizeof(pads));
    char buffer[HEADER_DIAGNOSTICS_JSON_MAX_BYTES];
    assert(header_diagnostics_json(NULL, 42) == 0);
    assert(header_diagnostics_json(buffer, 0) == 0);
    assert(header_diagnostics_json(buffer, sizeof(buffer)) > 0);
    puts(buffer);
    for (int gpio = 0; gpio < 55; ++gpio) {
        assert(enables[gpio] == 0U && reads[gpio] == 0U);
    }
    assert(header_diagnostics_init() == (scenario == 0));
    assert(header_diagnostics_init() == (scenario == 0)); /* idempotent */
    const size_t length = header_diagnostics_json(buffer, sizeof(buffer));
    assert(length > 0 && length < sizeof(buffer));
    puts(buffer);
    for (int gpio = 0; gpio < 55; ++gpio) {
        const int position = hardware_profile_p1_position(gpio);
        const bool allowed = position > 0 &&
            hardware_profile_p1_pin((size_t)position - 1U)->diagnostic_input;
        const bool failed = scenario != 0 && gpio == 2;
        assert(reads[gpio] == (allowed && !failed ? 1U : 0U));
        assert(enables[gpio] == (allowed && !before[gpio].ie &&
                                !(scenario == 1 && gpio == 2) ? 1U : 0U));
        gpio_io_config_t expected = before[gpio];
        if (allowed && !failed) {
            expected.ie = true;
        }
        if (!failed) {
            assert(memcmp(&expected, &pads[gpio], sizeof(expected)) == 0);
        }
    }
    assert(header_diagnostics_json(buffer, 1) == 0);
    assert(header_diagnostics_json(buffer, 100) == 0);
    assert(header_diagnostics_json(buffer, length) == 0);
    assert(header_diagnostics_json(buffer, length + 1) == length);
    pads[4].ie = false;
    config_error_gpio = 3;
    assert(header_diagnostics_json(buffer, sizeof(buffer)) > 0);
    puts(buffer);
    assert(!pads[4].ie); /* GET must not re-enable a buffer disabled later. */
    assert(enables[4] == 1U);
    return 0;
}
