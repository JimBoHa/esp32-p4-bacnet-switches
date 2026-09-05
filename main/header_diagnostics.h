#pragma once

#include <stdbool.h>
#include <stddef.h>

#define HEADER_DIAGNOSTICS_JSON_MAX_BYTES 16384U

/* Once at startup, after switch/Ethernet initialization and before HTTPS.
 * Only enables input buffers for the board's allowlisted header GPIOs.
 * Never changes mux, pulls, output drivers, interrupts, or USB configuration.
 * False indicates a diagnostic initialization failure, not a fatal boot error.
 */
bool header_diagnostics_init(void);

/* Read-only, sequential instantaneous samples, not a debounce/edge monitor.
 * Writes one JSON object and returns its length, or 0 on insufficient capacity.
 * Unreadable pins have raw_level:null with an explicit status.
 */
size_t header_diagnostics_json(char *buffer, size_t capacity);
