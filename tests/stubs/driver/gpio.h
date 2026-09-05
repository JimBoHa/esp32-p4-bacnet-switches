#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
typedef struct {
    bool pu, pd, ie, oe, oe_ctrl_by_periph, oe_inv, od, slp_sel;
    unsigned drv, fun_sel, sig_out;
} gpio_io_config_t;

esp_err_t gpio_get_io_config(int gpio, gpio_io_config_t *config);
esp_err_t gpio_input_enable(int gpio);
int gpio_get_level(int gpio);
