#include "switch_inputs.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define INPUT_POLL_MS 10U

static const char *TAG = "switch_inputs";
static const gpio_num_t INPUT_GPIOS[SWITCH_INPUT_COUNT] = {
    (gpio_num_t)CONFIG_TOGGLE_INPUT_1_GPIO,
    (gpio_num_t)CONFIG_TOGGLE_INPUT_2_GPIO,
    (gpio_num_t)CONFIG_TOGGLE_INPUT_3_GPIO,
};

static atomic_uint_fast32_t stable_input_bits;
static switch_input_pad_config_t startup_pad_configs[SWITCH_INPUT_COUNT];
static bool startup_raw_levels[SWITCH_INPUT_COUNT];
static switch_input_pad_config_t configured_pad_configs[SWITCH_INPUT_COUNT];
static bool configured_raw_levels[SWITCH_INPUT_COUNT];

static switch_input_pad_config_t read_pad_config(gpio_num_t gpio)
{
    switch_input_pad_config_t snapshot = {0};
    gpio_io_config_t config = {0};

    if (gpio_get_io_config(gpio, &config) != ESP_OK) {
        return snapshot;
    }
    snapshot.valid = true;
    snapshot.function_select = config.fun_sel;
    snapshot.output_signal = config.sig_out;
    snapshot.drive_capability = (uint32_t)config.drv;
    snapshot.pull_up_enabled = config.pu;
    snapshot.pull_down_enabled = config.pd;
    snapshot.input_enabled = config.ie;
    snapshot.output_enabled = config.oe;
    snapshot.output_enable_controlled_by_peripheral = config.oe_ctrl_by_periph;
    snapshot.output_enable_inverted = config.oe_inv;
    snapshot.open_drain_enabled = config.od;
    snapshot.sleep_select_enabled = config.slp_sel;
    return snapshot;
}

static bool gpio_reserved_for_board_ethernet(int gpio)
{
    static const int RESERVED_GPIOS[] = {
        28, /* RMII CRS_DV */
        29, /* RMII RXD0 */
        30, /* RMII RXD1 */
        31, /* SMI MDC */
        34, /* RMII TXD0 */
        35, /* RMII TXD1 */
        49, /* RMII TX_EN */
        50, /* RMII reference clock */
        51, /* PHY reset */
        52, /* SMI MDIO */
    };

    for (size_t index = 0;
         index < sizeof(RESERVED_GPIOS) / sizeof(RESERVED_GPIOS[0]);
         ++index) {
        if (gpio == RESERVED_GPIOS[index]) {
            return true;
        }
    }
    return false;
}

static uint32_t read_input_bits(void)
{
    uint32_t bits = 0;

    for (size_t index = 0; index < SWITCH_INPUT_COUNT; ++index) {
        if (gpio_get_level(INPUT_GPIOS[index]) != 0) {
            bits |= (1U << index);
        }
    }
    return bits;
}

static void store_input(size_t index, bool active)
{
    const uint_fast32_t mask = (uint_fast32_t)1U << index;
    uint_fast32_t old_bits;
    uint_fast32_t new_bits;

    do {
        old_bits = atomic_load_explicit(&stable_input_bits, memory_order_relaxed);
        new_bits = active ? old_bits | mask : old_bits & ~mask;
    } while (!atomic_compare_exchange_weak_explicit(
        &stable_input_bits,
        &old_bits,
        new_bits,
        memory_order_release,
        memory_order_relaxed));
}

static void switch_poll_task(void *argument)
{
    (void)argument;
    bool stable[SWITCH_INPUT_COUNT];
    bool candidate[SWITCH_INPUT_COUNT];
    uint32_t candidate_time_ms[SWITCH_INPUT_COUNT] = {0};
    const uint32_t initial_bits = read_input_bits();

    for (size_t index = 0; index < SWITCH_INPUT_COUNT; ++index) {
        stable[index] = (initial_bits & (1U << index)) != 0;
        candidate[index] = stable[index];
        store_input(index, stable[index]);
        ESP_LOGI(
            TAG,
            "GPIO%d initial state: %s",
            (int)INPUT_GPIOS[index],
            stable[index] ? "ACTIVE" : "INACTIVE");
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(INPUT_POLL_MS));
        const uint32_t sampled_bits = read_input_bits();

        for (size_t index = 0; index < SWITCH_INPUT_COUNT; ++index) {
            const bool sampled = (sampled_bits & (1U << index)) != 0;

            if (sampled == stable[index]) {
                candidate[index] = sampled;
                candidate_time_ms[index] = 0;
                continue;
            }
            if (sampled != candidate[index]) {
                candidate[index] = sampled;
                candidate_time_ms[index] = INPUT_POLL_MS;
                continue;
            }

            if (candidate_time_ms[index] < CONFIG_TOGGLE_DEBOUNCE_MS) {
                candidate_time_ms[index] += INPUT_POLL_MS;
            }
            if (candidate_time_ms[index] >= CONFIG_TOGGLE_DEBOUNCE_MS) {
                stable[index] = sampled;
                candidate_time_ms[index] = 0;
                store_input(index, stable[index]);
                ESP_LOGI(
                    TAG,
                    "GPIO%d changed to %s",
                    (int)INPUT_GPIOS[index],
                    stable[index] ? "ACTIVE" : "INACTIVE");
            }
        }
    }
}

void switch_inputs_init(void)
{
    uint64_t pin_bit_mask = 0;

    for (size_t index = 0; index < SWITCH_INPUT_COUNT; ++index) {
        if (gpio_reserved_for_board_ethernet((int)INPUT_GPIOS[index])) {
            ESP_LOGE(
                TAG,
                "GPIO%d conflicts with board Ethernet wiring",
                (int)INPUT_GPIOS[index]);
            abort();
        }
        for (size_t prior = 0; prior < index; ++prior) {
            if (INPUT_GPIOS[index] == INPUT_GPIOS[prior]) {
                ESP_LOGE(
                    TAG,
                    "toggle inputs must use different GPIOs (GPIO%d repeats)",
                    (int)INPUT_GPIOS[index]);
                abort();
            }
        }
        pin_bit_mask |= 1ULL << (unsigned)INPUT_GPIOS[index];
    }

    for (size_t index = 0; index < SWITCH_INPUT_COUNT; ++index) {
        startup_pad_configs[index] = read_pad_config(INPUT_GPIOS[index]);
        ESP_ERROR_CHECK(gpio_input_enable(INPUT_GPIOS[index]));
        startup_raw_levels[index] =
            gpio_get_level(INPUT_GPIOS[index]) != 0;
    }

    const gpio_config_t config = {
        .pin_bit_mask = pin_bit_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));

    /* Repeat the safety-critical settings through public APIs. ESP-IDF 5.5
       fixed the output-disable path used by gpio_set_direction() so
       GPIO_ENABLE_REG, rather than a peripheral, controls output enable when
       the pad function is GPIO. */
    for (size_t index = 0; index < SWITCH_INPUT_COUNT; ++index) {
        ESP_ERROR_CHECK(
            gpio_set_direction(INPUT_GPIOS[index], GPIO_MODE_INPUT));
        ESP_ERROR_CHECK(
            gpio_set_pull_mode(INPUT_GPIOS[index], GPIO_PULLDOWN_ONLY));
    }
    vTaskDelay(pdMS_TO_TICKS(1));

    for (size_t index = 0; index < SWITCH_INPUT_COUNT; ++index) {
        configured_pad_configs[index] = read_pad_config(INPUT_GPIOS[index]);
        configured_raw_levels[index] =
            gpio_get_level(INPUT_GPIOS[index]) != 0;
    }

    atomic_init(&stable_input_bits, read_input_bits());
    const BaseType_t task_result = xTaskCreate(
        switch_poll_task,
        "switch_inputs",
        3072,
        NULL,
        5,
        NULL);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "failed to create switch input task");
        abort();
    }
}

bool switch_input_get(size_t index)
{
    if (index >= SWITCH_INPUT_COUNT) {
        return false;
    }
    const uint_fast32_t bits =
        atomic_load_explicit(&stable_input_bits, memory_order_acquire);
    return (bits & ((uint_fast32_t)1U << index)) != 0;
}

bool switch_input_diagnostics_get(
    size_t index,
    switch_input_diagnostics_t *diagnostics)
{
    if (index >= SWITCH_INPUT_COUNT || diagnostics == NULL) {
        return false;
    }

    *diagnostics = (switch_input_diagnostics_t){
        .gpio = (int)INPUT_GPIOS[index],
        .startup_config = startup_pad_configs[index],
        .startup_raw_after_input_enable = startup_raw_levels[index],
        .configured_config = configured_pad_configs[index],
        .configured_raw = configured_raw_levels[index],
        .current_config = read_pad_config(INPUT_GPIOS[index]),
        .current_raw = gpio_get_level(INPUT_GPIOS[index]) != 0,
        .stable = switch_input_get(index),
    };
    return true;
}
