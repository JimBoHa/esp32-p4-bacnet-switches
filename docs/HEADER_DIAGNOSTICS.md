# P1 header diagnostics

Supported board: **Waveshare ESP32-P4-POE-ETH**, not an ESP32 DevKit or the
ESP32-P4-WIFI6-POE-ETH. The connector has 40 positions (2x20). Its physical
layout resembles a Pico, but its GPIO numbering is different.

Mapping verified against the manufacturer's [board documentation](https://docs.waveshare.com/ESP32-P4-ETH)
and [schematic](https://files.waveshare.com/wiki/ESP32-P4-ETH/ESP32-P4-ETH-datasheet.pdf),
page 1, P1 `PICO-2*20 IO`. The SoC symbol resolves `ESP_I2C_SCL` to GPIO8 and
`ESP_I2C_SDA` to GPIO7.

## Coverage and safety

Open `/diagnostics` without logging in. **P1 header: all 40 positions** refreshes
with status every five seconds, or with **Refresh now**. The HIGH summary gives
both physical positions and GPIO numbers. `/ota/status` and `/diagnostics/report`
include `header_diagnostics` with all 40 positions in physical-number order.

- 25 header GPIOs are sampled using digital input buffers. At startup, after
  switch and Ethernet driver initialization, `gpio_input_enable()` enables only
  their input buffers if needed. No output level, output enable, pull resistor,
  mux, interrupt, USB, Ethernet, flash, or PSRAM setting is changed. Before/after
  pad configuration is checked, excluding the intended input-enable change.
- GPIO24 and GPIO25 are reserved for USB/USB-JTAG. They are **not** reconfigured
  or sampled, even if their GPIO input flags happen to be set. Power, ground,
  reset, and regulator-enable positions are not GPIO inputs. These 15 positions
  have `raw_level: null`, not fabricated LOW/HIGH values.
- Uninitialized, failed, or subsequently disabled GPIO inputs also report null
  with an explicit status. GET requests never re-enable them or mutate pins.
- Samples are sequential and instantaneous, with capture/completion uptime.
  They are not an atomic logic-analyzer capture, edge counter, debounce result,
  or voltage measurement. Floating inputs can be HIGH or change arbitrarily;
  I2C has board pull-ups and can normally read HIGH. A HIGH alone does not prove
  an external switch is closed. Existing pad pull/output settings are shown.
- Only GPIO20/21/22 remain debounced BACnet switch channels, with their original
  pull-downs, object identifiers, history, and polarity. No BACnet map changes.
- GPIO inputs are **3.3 V only**. Never connect a GPIO to a 5 V header rail or
  external 12/24 V supply. Do not wire switches to reset or regulator enable.
  Use suitable external bias/protection and verify wiring with power removed.

## Complete physical map

| P1 position | Board net | ESP32 GPIO | Sampling |
| --- | --- | --- | --- |
| 1 | GPIO54 | 54 | Digital |
| 2 | GPIO19 | 19 | Digital |
| 3 | GND | - | Not GPIO |
| 4 | GPIO18 | 18 | Digital |
| 5 | GPIO17 | 17 | Digital |
| 6 | GPIO16 | 16 | Digital |
| 7 | GPIO15 | 15 | Digital |
| 8 | GND | - | Not GPIO |
| 9 | GPIO14 | 14 | Digital |
| 10 | GPIO6 | 6 | Digital |
| 11 | GPIO5 | 5 | Digital |
| 12 | GPIO4 | 4 | Digital |
| 13 | GND | - | Not GPIO |
| 14 | GPIO3 | 3 | Digital |
| 15 | GPIO2 | 2 | Digital |
| 16 | ESP_I2C_SCL | 8 | Digital; shared I2C |
| 17 | ESP_I2C_SDA | 7 | Digital; shared I2C |
| 18 | GND | - | Not GPIO |
| 19 | USB1P1_N | 24 | Reserved USB D- |
| 20 | USB1P1_P | 25 | Reserved USB D+ |
| 21 | GPIO48 | 48 | Digital |
| 22 | GPIO47 | 47 | Digital |
| 23 | GND | - | Not GPIO |
| 24 | GPIO46 | 46 | Digital |
| 25 | GPIO33 | 33 | Digital |
| 26 | GPIO32 | 32 | Digital |
| 27 | GPIO27 | 27 | Digital |
| 28 | GND | - | Not GPIO |
| 29 | GPIO26 | 26 | Digital |
| 30 | ESP_EN | - | Reset/enable; not GPIO |
| 31 | GPIO23 | 23 | Digital |
| 32 | GPIO22 | 22 | Digital; configured switch |
| 33 | GND | - | Not GPIO |
| 34 | GPIO21 | 21 | Digital; configured switch |
| 35 | GPIO20 | 20 | Digital; configured switch |
| 36 | ESP_3V3 | - | Supply; not measured |
| 37 | 3V3_EN | - | Regulator enable; not GPIO |
| 38 | GND | - | Not GPIO |
| 39 | VCC_5V | - | Supply; not measured |
| 40 | VCC1_5V | - | Supply; not measured |
