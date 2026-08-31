#pragma once

#include <stdbool.h>
#include <stddef.h>

#define OTA_BEARER_PREFIX "Bearer "
#define OTA_TOKEN_MIN_LENGTH 32U
#define OTA_TOKEN_MAX_LENGTH 128U
#define OTA_AUTHORIZATION_MAX_LENGTH \
    ((sizeof(OTA_BEARER_PREFIX) - 1U) + OTA_TOKEN_MAX_LENGTH)

bool ota_token_configuration_valid(const char *token);

bool ota_authorization_valid(
    const char *authorization,
    size_t authorization_length,
    const char *token);
