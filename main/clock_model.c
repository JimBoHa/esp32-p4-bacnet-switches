#include "clock_model.h"

bool clock_model_sync(clock_model_t *clock, uint64_t uptime_ms, uint64_t unix_ms)
{
    if (clock == NULL) {
        return false;
    }
    if (unix_ms < CLOCK_MIN_UNIX_MS || unix_ms >= CLOCK_MAX_UNIX_MS ||
        (clock->sync_count != 0U && uptime_ms < clock->last_sync_uptime_ms)) {
        if (clock->rejected_sync_count != UINT32_MAX) clock->rejected_sync_count++;
        return false;
    }
    clock->last_sync_uptime_ms = uptime_ms;
    clock->last_sync_unix_ms = unix_ms;
    if (clock->sync_count != UINT32_MAX) clock->sync_count++;
    return true;
}

clock_stamp_t clock_model_stamp(const clock_model_t *clock, uint64_t uptime_ms)
{
    clock_stamp_t stamp = {.uptime_ms = uptime_ms};
    if (clock == NULL || clock->sync_count == 0U ||
        uptime_ms < clock->last_sync_uptime_ms ||
        clock->last_sync_unix_ms < CLOCK_MIN_UNIX_MS ||
        clock->last_sync_unix_ms >= CLOCK_MAX_UNIX_MS) {
        return stamp;
    }
    const uint64_t age = uptime_ms - clock->last_sync_uptime_ms;
    if (age >= CLOCK_MAX_UNIX_MS - clock->last_sync_unix_ms) return stamp;
    stamp.unix_ms = clock->last_sync_unix_ms + age;
    stamp.quality = age > CLOCK_STALE_AFTER_MS ? CLOCK_STALE : CLOCK_SYNCHRONIZED;
    return stamp;
}

const char *clock_quality_name(clock_quality_t quality)
{
    switch (quality) {
    case CLOCK_SYNCHRONIZED: return "synchronized";
    case CLOCK_STALE: return "stale";
    default: return "unsynchronized";
    }
}

static bool alphanumeric(char character)
{
    return (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9');
}

bool clock_server_name_valid(const char *server)
{
    if (server == NULL) return false;
    if (server[0] == '\0') return true; /* DHCP option 42 */
    size_t label_length = 0U;
    char previous = '\0';
    for (size_t index = 0U; index <= CLOCK_SERVER_MAX_LENGTH; ++index) {
        const char character = server[index];
        if (character == '\0') return label_length > 0U && alphanumeric(previous);
        if (character == '.') {
            if (label_length == 0U || !alphanumeric(previous)) return false;
            label_length = 0U;
        } else {
            if ((!alphanumeric(character) && character != '-') ||
                (label_length == 0U && !alphanumeric(character)) ||
                ++label_length > 63U) return false;
        }
        previous = character;
    }
    return false;
}
