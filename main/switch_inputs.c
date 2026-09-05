#include "switch_inputs.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "diagnostics.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#ifndef CONFIG_TOGGLE_INPUT_1_ACTIVE_LOW
#define CONFIG_TOGGLE_INPUT_1_ACTIVE_LOW 0
#endif
#ifndef CONFIG_TOGGLE_INPUT_2_ACTIVE_LOW
#define CONFIG_TOGGLE_INPUT_2_ACTIVE_LOW 0
#endif
#ifndef CONFIG_TOGGLE_INPUT_3_ACTIVE_LOW
#define CONFIG_TOGGLE_INPUT_3_ACTIVE_LOW 0
#endif

#define INPUT_POLL_MS 10U

static const char *TAG = "switch_inputs";
static const gpio_num_t INPUT_GPIOS[SWITCH_INPUT_COUNT] = {
    (gpio_num_t)CONFIG_TOGGLE_INPUT_1_GPIO,
    (gpio_num_t)CONFIG_TOGGLE_INPUT_2_GPIO,
    (gpio_num_t)CONFIG_TOGGLE_INPUT_3_GPIO,
};
static const bool INPUT_ACTIVE_LOW[SWITCH_INPUT_COUNT] = {
    CONFIG_TOGGLE_INPUT_1_ACTIVE_LOW,
    CONFIG_TOGGLE_INPUT_2_ACTIVE_LOW,
    CONFIG_TOGGLE_INPUT_3_ACTIVE_LOW,
};

static atomic_uint_fast32_t stable_input_bits;
static atomic_uint_fast32_t input_fault_bits;
static atomic_bool self_test_active;
static atomic_uint_fast32_t transition_counts[SWITCH_INPUT_COUNT];
static uint64_t last_transition_ms[SWITCH_INPUT_COUNT];
static portMUX_TYPE transition_time_lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool self_test_run[SWITCH_INPUT_COUNT];
static atomic_bool self_test_passed[SWITCH_INPUT_COUNT];
static atomic_bool self_test_pull_down_levels[SWITCH_INPUT_COUNT];
static atomic_bool self_test_pull_up_levels[SWITCH_INPUT_COUNT];
static atomic_bool self_test_pull_down_stable[SWITCH_INPUT_COUNT];
static atomic_bool self_test_pull_up_stable[SWITCH_INPUT_COUNT];
static atomic_uint_fast32_t self_test_classifications[SWITCH_INPUT_COUNT];
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
        const bool raw = gpio_get_level(INPUT_GPIOS[index]) != 0;
        const bool active = raw != INPUT_ACTIVE_LOW[index];
        if (active) {
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
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        diagnostics_task_watchdog_subscribe(
            DIAGNOSTICS_TASK_SWITCH_INPUTS));
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
        diagnostics_task_heartbeat(DIAGNOSTICS_TASK_SWITCH_INPUTS);
        if (atomic_load_explicit(
                &self_test_active, memory_order_acquire)) {
            continue;
        }
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
                atomic_fetch_add_explicit(
                    &transition_counts[index], 1U, memory_order_relaxed);
                portENTER_CRITICAL(&transition_time_lock);
                last_transition_ms[index] =
                    (uint64_t)esp_timer_get_time() / 1000U;
                portEXIT_CRITICAL(&transition_time_lock);
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

    atomic_init(&input_fault_bits, 0U);
    atomic_init(&self_test_active, false);
    for (size_t index = 0; index < SWITCH_INPUT_COUNT; ++index) {
        atomic_init(&transition_counts[index], 0U);
        last_transition_ms[index] = 0U;
        atomic_init(&self_test_run[index], false);
        atomic_init(&self_test_passed[index], false);
        atomic_init(&self_test_pull_down_levels[index], false);
        atomic_init(&self_test_pull_up_levels[index], false);
        atomic_init(&self_test_pull_down_stable[index], false);
        atomic_init(&self_test_pull_up_stable[index], false);
        atomic_init(
            &self_test_classifications[index], INPUT_LINE_NOT_TESTED);
        startup_pad_configs[index] = read_pad_config(INPUT_GPIOS[index]);
        ESP_ERROR_CHECK(
            gpio_set_direction(INPUT_GPIOS[index], GPIO_MODE_INPUT));
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

bool switch_input_faulted(size_t index)
{
    if (index >= SWITCH_INPUT_COUNT) {
        return true;
    }
    const uint_fast32_t bits =
        atomic_load_explicit(&input_fault_bits, memory_order_acquire);
    return (bits & ((uint_fast32_t)1U << index)) != 0U;
}

bool switch_input_active_low(size_t index)
{
    return index < SWITCH_INPUT_COUNT && INPUT_ACTIVE_LOW[index];
}

typedef struct {
    bool stable;
    bool level;
} gpio_level_sample_t;

static gpio_level_sample_t sample_gpio_level(gpio_num_t gpio)
{
    unsigned high_samples = 0U;
    for (unsigned sample = 0; sample < 8U; ++sample) {
        if (gpio_get_level(gpio) != 0) {
            high_samples++;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return (gpio_level_sample_t){
        .stable = high_samples <= 2U || high_samples >= 6U,
        .level = high_samples >= 6U,
    };
}

esp_err_t switch_inputs_run_self_test(void)
{
    if (atomic_exchange_explicit(
            &self_test_active, true, memory_order_acq_rel)) {
        return ESP_ERR_INVALID_STATE;
    }

    uint_fast32_t fault_bits = 0U;
    for (size_t index = 0; index < SWITCH_INPUT_COUNT; ++index) {
        const gpio_num_t gpio = INPUT_GPIOS[index];
        gpio_level_sample_t pull_down = {0};
        gpio_level_sample_t pull_up = {0};
        esp_err_t result = gpio_set_direction(gpio, GPIO_MODE_INPUT);
        if (result == ESP_OK) {
            result = gpio_set_pull_mode(gpio, GPIO_PULLDOWN_ONLY);
        }
        if (result == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(3));
            pull_down = sample_gpio_level(gpio);
            result = gpio_set_pull_mode(gpio, GPIO_PULLUP_ONLY);
        }
        if (result == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(3));
            pull_up = sample_gpio_level(gpio);
        }
        const esp_err_t restore_result =
            gpio_set_pull_mode(gpio, GPIO_PULLDOWN_ONLY);
        if (result == ESP_OK) {
            result = restore_result;
        }

        const input_line_classification_t classification = result == ESP_OK
            ? input_line_classify(
                  pull_down.stable,
                  pull_down.level,
                  pull_up.stable,
                  pull_up.level)
            : INPUT_LINE_UNSTABLE;
        const bool passed = input_line_classification_valid(classification);
        atomic_store_explicit(
            &self_test_pull_down_levels[index],
            pull_down.level,
            memory_order_release);
        atomic_store_explicit(
            &self_test_pull_up_levels[index],
            pull_up.level,
            memory_order_release);
        atomic_store_explicit(
            &self_test_pull_down_stable[index],
            pull_down.stable,
            memory_order_release);
        atomic_store_explicit(
            &self_test_pull_up_stable[index],
            pull_up.stable,
            memory_order_release);
        atomic_store_explicit(
            &self_test_classifications[index],
            (uint32_t)classification,
            memory_order_release);
        atomic_store_explicit(
            &self_test_passed[index], passed, memory_order_release);
        atomic_store_explicit(
            &self_test_run[index], true, memory_order_release);
        if (!passed) {
            fault_bits |= (uint_fast32_t)1U << index;
        }
    }
    atomic_store_explicit(
        &input_fault_bits, fault_bits, memory_order_release);
    atomic_store_explicit(&self_test_active, false, memory_order_release);
    return ESP_OK;
}

bool switch_input_diagnostics_get(
    size_t index,
    switch_input_diagnostics_t *diagnostics)
{
    if (index >= SWITCH_INPUT_COUNT || diagnostics == NULL) {
        return false;
    }

    portENTER_CRITICAL(&transition_time_lock);
    const uint64_t last_transition_uptime_ms = last_transition_ms[index];
    portEXIT_CRITICAL(&transition_time_lock);

    *diagnostics = (switch_input_diagnostics_t){
        .gpio = (int)INPUT_GPIOS[index],
        .active_low = INPUT_ACTIVE_LOW[index],
        .debounce_ms = CONFIG_TOGGLE_DEBOUNCE_MS,
        .startup_config = startup_pad_configs[index],
        .startup_raw_after_input_enable = startup_raw_levels[index],
        .configured_config = configured_pad_configs[index],
        .configured_raw = configured_raw_levels[index],
        .current_config = read_pad_config(INPUT_GPIOS[index]),
        .current_raw = gpio_get_level(INPUT_GPIOS[index]) != 0,
        .stable = switch_input_get(index),
        .transition_count = (uint32_t)atomic_load_explicit(
            &transition_counts[index], memory_order_relaxed),
        .last_transition_uptime_ms = last_transition_uptime_ms,
        .self_test_run = atomic_load_explicit(
            &self_test_run[index], memory_order_acquire),
        .self_test_passed = atomic_load_explicit(
            &self_test_passed[index], memory_order_acquire),
        .self_test_pull_down_level = atomic_load_explicit(
            &self_test_pull_down_levels[index], memory_order_acquire),
        .self_test_pull_up_level = atomic_load_explicit(
            &self_test_pull_up_levels[index], memory_order_acquire),
        .self_test_pull_down_stable = atomic_load_explicit(
            &self_test_pull_down_stable[index], memory_order_acquire),
        .self_test_pull_up_stable = atomic_load_explicit(
            &self_test_pull_up_stable[index], memory_order_acquire),
        .self_test_classification =
            (input_line_classification_t)atomic_load_explicit(
                &self_test_classifications[index], memory_order_acquire),
    };
    return true;
}
