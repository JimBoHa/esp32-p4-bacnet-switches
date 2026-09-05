#include "cov_retry_cache.h"

#include <string.h>

bool cov_retry_cache_capture(
    cov_retry_cache_t *cache,
    const uint8_t *payload,
    size_t payload_length)
{
    if (cache == NULL || payload == NULL || payload_length == 0U ||
        payload_length > sizeof(cache->bytes)) {
        return false;
    }
    memcpy(cache->bytes, payload, payload_length);
    cache->length = payload_length;
    return true;
}

const uint8_t *cov_retry_cache_data(const cov_retry_cache_t *cache)
{
    return cache != NULL && cache->length > 0U ? cache->bytes : NULL;
}

size_t cov_retry_cache_length(const cov_retry_cache_t *cache)
{
    return cache != NULL ? cache->length : 0U;
}

void cov_retry_cache_clear(cov_retry_cache_t *cache)
{
    if (cache != NULL) {
        memset(cache, 0, sizeof(*cache));
    }
}
