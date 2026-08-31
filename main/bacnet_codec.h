#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BACNET_IP_DEFAULT_PORT 47808U
#define BACNET_MAX_APDU 1476U
#define BACNET_MAX_REQUEST_BYTES 512U
#define BACNET_BINARY_INPUT_COUNT 2U
#define BACNET_MAX_INSTANCE 4194303U

typedef struct {
    uint32_t device_instance;
    uint16_t vendor_identifier;
    const char *device_name;
    const char *vendor_name;
    const char *model_name;
    const char *firmware_revision;
    const char *description;
    uint32_t database_revision;
    uint32_t binary_input_instances[BACNET_BINARY_INPUT_COUNT];
    const char *binary_input_names[BACNET_BINARY_INPUT_COUNT];
    const char *binary_input_descriptions[BACNET_BINARY_INPUT_COUNT];
    bool binary_input_values[BACNET_BINARY_INPUT_COUNT];
} bacnet_device_state_t;

typedef enum {
    BACNET_PACKET_MALFORMED,
    BACNET_PACKET_IGNORED,
    BACNET_PACKET_WHO_IS,
    BACNET_PACKET_READ_PROPERTY,
} bacnet_packet_kind_t;

typedef struct {
    bacnet_packet_kind_t kind;
    size_t response_length;
} bacnet_packet_result_t;

size_t bacnet_encode_i_am(
    const bacnet_device_state_t *state,
    bool broadcast,
    uint8_t *response,
    size_t response_capacity);

bacnet_packet_result_t bacnet_handle_packet(
    const uint8_t *frame,
    size_t frame_length,
    const bacnet_device_state_t *state,
    uint8_t *response,
    size_t response_capacity);
