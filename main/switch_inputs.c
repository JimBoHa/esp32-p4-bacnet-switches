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
};

static atomic_uint_fast32_t stable_input_bits;

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
    if (CONFIG_TOGGLE_INPUT_1_GPIO == CONFIG_TOGGLE_INPUT_2_GPIO) {
        ESP_LOGE(TAG, "toggle inputs must use different GPIOs");
        abort();
    }
    if (gpio_reserved_for_board_ethernet(CONFIG_TOGGLE_INPUT_1_GPIO) ||
        gpio_reserved_for_board_ethernet(CONFIG_TOGGLE_INPUT_2_GPIO)) {
        ESP_LOGE(TAG, "toggle input conflicts with board Ethernet wiring");
        abort();
    }

    const gpio_config_t config = {
        .pin_bit_mask =
            (1ULL << CONFIG_TOGGLE_INPUT_1_GPIO) |
            (1ULL << CONFIG_TOGGLE_INPUT_2_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));

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
