#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COV_RETRY_PAYLOAD_CAPACITY 128U

typedef struct {
    uint8_t bytes[COV_RETRY_PAYLOAD_CAPACITY];
    size_t length;
} cov_retry_cache_t;

bool cov_retry_cache_capture(
    cov_retry_cache_t *cache,
    const uint8_t *payload,
    size_t payload_length);

const uint8_t *cov_retry_cache_data(const cov_retry_cache_t *cache);
size_t cov_retry_cache_length(const cov_retry_cache_t *cache);
void cov_retry_cache_clear(cov_retry_cache_t *cache);
