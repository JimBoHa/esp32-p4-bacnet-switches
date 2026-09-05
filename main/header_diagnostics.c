#include "header_diagnostics.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "hardware_profile.h"

typedef struct {
    bool enabled_by_diagnostics;
    bool configuration_preserved;
    esp_err_t error;
} pin_initialization_t;

static pin_initialization_t initialization[HARDWARE_PROFILE_P1_COUNT];
static bool initialized;
static bool initialization_ok;

static const char *boolean(bool value)
{
    return value ? "true" : "false";
}

static bool same_except_input(const gpio_io_config_t *a, const gpio_io_config_t *b)
{
    return a->pu == b->pu && a->pd == b->pd && a->oe == b->oe &&
        a->oe_ctrl_by_periph == b->oe_ctrl_by_periph && a->oe_inv == b->oe_inv &&
        a->od == b->od && a->drv == b->drv && a->fun_sel == b->fun_sel &&
        a->sig_out == b->sig_out && a->slp_sel == b->slp_sel;
}

bool header_diagnostics_init(void)
{
    if (initialized) {
        return initialization_ok;
    }
    initialization_ok = true;
    for (size_t index = 0; index < HARDWARE_PROFILE_P1_COUNT; ++index) {
        const hardware_profile_pin_t *pin = hardware_profile_p1_pin(index);
        if (!pin->diagnostic_input) {
            continue;
        }
        pin_initialization_t *state = &initialization[index];
        gpio_io_config_t before = {0}, after = {0};
        state->error = gpio_get_io_config(pin->gpio, &before);
        if (state->error == ESP_OK && !before.ie) {
            state->error = gpio_input_enable(pin->gpio);
            state->enabled_by_diagnostics = state->error == ESP_OK;
        }
        if (state->error == ESP_OK) {
            state->error = gpio_get_io_config(pin->gpio, &after);
            state->configuration_preserved = state->error == ESP_OK &&
                same_except_input(&before, &after);
            if (state->error == ESP_OK &&
                (!after.ie || !state->configuration_preserved)) {
                state->error = ESP_FAIL;
            }
        }
        initialization_ok = initialization_ok && state->error == ESP_OK;
    }
    initialized = true;
    return initialization_ok;
}

static bool append(char *buffer, size_t capacity, size_t *length,
                   const char *format, ...)
{
    if (*length >= capacity) {
        return false;
    }
    va_list args;
    va_start(args, format);
    const int count = vsnprintf(buffer + *length, capacity - *length, format, args);
    va_end(args);
    if (count < 0 || (size_t)count >= capacity - *length) {
        return false;
    }
    *length += (size_t)count;
    return true;
}

size_t header_diagnostics_json(char *buffer, size_t capacity)
{
    if (buffer == NULL || capacity == 0U) {
        return 0U;
    }
    size_t length = 0;
    unsigned readable_count = 0;
    if (!append(buffer, capacity, &length,
                "{\"header\":\"P1\",\"position_count\":40,\"gpio_count\":27,"
                "\"initialized\":%s,\"initialization_ok\":%s,"
                "\"sample_mode\":\"sequential-on-request\","
                "\"captured_uptime_ms\":%" PRIu64 ",\"pins\":[",
                boolean(initialized), boolean(initialization_ok),
                (uint64_t)(esp_timer_get_time() / 1000))) {
        return 0;
    }
    for (size_t index = 0; index < HARDWARE_PROFILE_P1_COUNT; ++index) {
        const hardware_profile_pin_t *pin = hardware_profile_p1_pin(index);
        const pin_initialization_t *state = &initialization[index];
        gpio_io_config_t config = {0};
        const char *status = "non-gpio";
        const char *raw_level = "null";
        bool config_valid = false;
        if (pin->gpio >= 0 && !pin->diagnostic_input) {
            status = "reserved-usb";
        } else if (pin->diagnostic_input) {
            config_valid = gpio_get_io_config(pin->gpio, &config) == ESP_OK;
            if (!initialized) {
                status = "not-initialized";
            } else if (state->error != ESP_OK) {
                status = "initialization-error";
            } else if (!config_valid) {
                status = "configuration-error";
            } else if (!config.ie) {
                status = "input-disabled";
            } else {
                status = "readable";
                raw_level = boolean(gpio_get_level(pin->gpio) != 0);
                ++readable_count;
            }
        }
        if (!append(buffer, capacity, &length,
                    "%s{\"position\":%u,\"label\":\"%s\",\"kind\":\"%s\","
                    "\"usage\":\"%s\",\"gpio\":",
                    index == 0U ? "" : ",", (unsigned)index + 1U,
                    pin->label, pin->kind, pin->usage) ||
            !append(buffer, capacity, &length, pin->gpio >= 0 ? "%d" : "null",
                    pin->gpio) ||
            !append(buffer, capacity, &length,
                    ",\"status\":\"%s\",\"raw_level\":%s,"
                    "\"input_enabled_by_diagnostics\":%s,"
                    "\"initialization_preserved_config\":%s,\"pad\":",
                    status, raw_level, boolean(state->enabled_by_diagnostics),
                    pin->diagnostic_input && initialized
                        ? boolean(state->configuration_preserved) : "null")) {
            return 0;
        }
        if (config_valid) {
            if (!append(buffer, capacity, &length,
                        "{\"input_enabled\":%s,\"output_enabled\":%s,"
                        "\"output_enable_controlled_by_peripheral\":%s,"
                        "\"pull_up\":%s,\"pull_down\":%s,\"function_select\":%u}",
                        boolean(config.ie), boolean(config.oe),
                        boolean(config.oe_ctrl_by_periph), boolean(config.pu),
                        boolean(config.pd), (unsigned)config.fun_sel)) {
                return 0;
            }
        } else if (!append(buffer, capacity, &length, "null")) {
            return 0;
        }
        if (!append(buffer, capacity, &length, "}")) {
            return 0;
        }
    }
    return append(buffer, capacity, &length,
                  "],\"readable_count\":%u,\"completed_uptime_ms\":%" PRIu64 "}",
                  readable_count, (uint64_t)(esp_timer_get_time() / 1000))
        ? length : 0U;
}
