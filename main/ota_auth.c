#include "ota_auth.h"

#include <stdint.h>
#include <string.h>

static size_t bounded_length(const char *value, size_t maximum)
{
    if (value == NULL) {
        return 0;
    }
    size_t length = 0;
    while (length < maximum && value[length] != '\0') {
        length++;
    }
    return length;
}

bool ota_token_configuration_valid(const char *token)
{
    const size_t length = bounded_length(token, OTA_TOKEN_MAX_LENGTH + 1U);
    return length >= OTA_TOKEN_MIN_LENGTH && length <= OTA_TOKEN_MAX_LENGTH;
}

bool ota_authorization_valid(
    const char *authorization,
    size_t authorization_length,
    const char *token)
{
    static const char PREFIX[] = OTA_BEARER_PREFIX;

    if (authorization == NULL || !ota_token_configuration_valid(token)) {
        return false;
    }
    const size_t prefix_length = sizeof(PREFIX) - 1U;
    const size_t token_length = strlen(token);
    if (authorization_length != prefix_length + token_length) {
        return false;
    }

    uint8_t difference = 0;
    for (size_t index = 0; index < prefix_length; ++index) {
        difference |= (uint8_t)authorization[index] ^ (uint8_t)PREFIX[index];
    }
    for (size_t index = 0; index < token_length; ++index) {
        difference |=
            (uint8_t)authorization[prefix_length + index] ^
            (uint8_t)token[index];
    }
    return difference == 0U;
}
