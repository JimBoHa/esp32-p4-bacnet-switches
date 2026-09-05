#include "network_config_model.h"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_crc.h"
#else
static uint32_t host_crc32_le(
    uint32_t crc,
    const uint8_t *data,
    size_t length)
{
    crc = ~crc;
    for (size_t index = 0U; index < length; ++index) {
        crc ^= data[index];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^
                (0xEDB88320U & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}
#define esp_crc32_le host_crc32_le
#endif

_Static_assert(
    offsetof(network_config_t, crc32) + sizeof(uint32_t) ==
        sizeof(network_config_t),
    "network configuration CRC must be the final field");

static void set_reason(char *reason, size_t capacity, const char *message)
{
    if (reason != NULL && capacity > 0U) {
        (void)snprintf(reason, capacity, "%s", message);
    }
}

static bool ascii_alphanumeric(char character)
{
    return (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9');
}

static bool hostname_valid(const char *hostname)
{
    const char *terminator = memchr(
        hostname, '\0', NETWORK_CONFIG_HOSTNAME_LENGTH);
    if (terminator == NULL) {
        return false;
    }
    const size_t length = (size_t)(terminator - hostname);
    if (length == 0U || length > 63U ||
        !ascii_alphanumeric(hostname[0]) ||
        !ascii_alphanumeric(hostname[length - 1U])) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        if (!ascii_alphanumeric(hostname[index]) && hostname[index] != '-') {
            return false;
        }
    }
    return true;
}

static uint32_t ipv4_value(const uint8_t address[4])
{
    return ((uint32_t)address[0] << 24U) |
        ((uint32_t)address[1] << 16U) |
        ((uint32_t)address[2] << 8U) |
        address[3];
}

static bool address_is_zero(const uint8_t address[4])
{
    return ipv4_value(address) == 0U;
}

static bool unicast_address(uint32_t address)
{
    const uint8_t first = (uint8_t)(address >> 24U);
    return address != 0U && address != UINT32_MAX && first != 0U &&
        first != 127U && first < 224U;
}

static bool static_addresses_valid(const network_config_t *config)
{
    const uint32_t address = ipv4_value(config->ipv4);
    const uint32_t netmask = ipv4_value(config->netmask);
    const uint32_t gateway = ipv4_value(config->gateway);
    const uint32_t dns = ipv4_value(config->dns);
    const uint32_t inverse_mask = ~netmask;
    if (!unicast_address(address) || !unicast_address(gateway) ||
        netmask == 0U || inverse_mask < 3U ||
        (inverse_mask & (inverse_mask + 1U)) != 0U) {
        return false;
    }
    const uint32_t network = address & netmask;
    const uint32_t broadcast = network | inverse_mask;
    if (address == network || address == broadcast ||
        (gateway & netmask) != network || gateway == network ||
        gateway == broadcast || gateway == address) {
        return false;
    }
    return dns == 0U || unicast_address(dns);
}

bool network_config_validate(
    const network_config_t *config,
    char *reason,
    size_t reason_capacity)
{
    if (config == NULL) {
        set_reason(reason, reason_capacity, "missing network configuration");
        return false;
    }
    if (!hostname_valid(config->hostname)) {
        set_reason(
            reason,
            reason_capacity,
            "hostname must be a 1-63 character RFC 1123 label");
        return false;
    }
    if (config->mode == NETWORK_ADDRESS_DHCP) {
        if (!address_is_zero(config->ipv4) ||
            !address_is_zero(config->netmask) ||
            !address_is_zero(config->gateway) ||
            !address_is_zero(config->dns)) {
            set_reason(
                reason,
                reason_capacity,
                "DHCP configuration must not contain static addresses");
            return false;
        }
    } else if (config->mode == NETWORK_ADDRESS_STATIC) {
        if (!static_addresses_valid(config)) {
            set_reason(
                reason,
                reason_capacity,
                "static IPv4, netmask, gateway, or DNS is invalid");
            return false;
        }
    } else {
        set_reason(reason, reason_capacity, "mode must be DHCP or static");
        return false;
    }
    set_reason(reason, reason_capacity, "ok");
    return true;
}

uint32_t network_config_crc32(const network_config_t *config)
{
    return config != NULL
        ? esp_crc32_le(
              0U,
              (const uint8_t *)config,
              offsetof(network_config_t, crc32))
        : 0U;
}

void network_config_finalize(network_config_t *config)
{
    if (config == NULL) {
        return;
    }
    config->magic = NETWORK_CONFIG_MAGIC;
    config->schema = NETWORK_CONFIG_SCHEMA;
    config->size = (uint16_t)sizeof(*config);
    memset(config->reserved, 0, sizeof(config->reserved));
    config->crc32 = network_config_crc32(config);
}

bool network_config_is_valid_blob(const network_config_t *config)
{
    return config != NULL && config->magic == NETWORK_CONFIG_MAGIC &&
        config->schema == NETWORK_CONFIG_SCHEMA &&
        config->size == sizeof(*config) &&
        config->crc32 == network_config_crc32(config) &&
        network_config_validate(config, NULL, 0U);
}

bool network_config_mutable_equal(
    const network_config_t *left,
    const network_config_t *right)
{
    return left != NULL && right != NULL && left->mode == right->mode &&
        strcmp(left->hostname, right->hostname) == 0 &&
        memcmp(left->ipv4, right->ipv4, sizeof(left->ipv4)) == 0 &&
        memcmp(left->netmask, right->netmask, sizeof(left->netmask)) == 0 &&
        memcmp(left->gateway, right->gateway, sizeof(left->gateway)) == 0 &&
        memcmp(left->dns, right->dns, sizeof(left->dns)) == 0;
}
