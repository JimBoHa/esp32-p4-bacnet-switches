#include "bacnet_codec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    BACNET_BVLC_TYPE = 0x81,
    BACNET_BVLC_ORIGINAL_UNICAST_NPDU = 0x0A,
    BACNET_BVLC_ORIGINAL_BROADCAST_NPDU = 0x0B,
    BACNET_OBJECT_BINARY_INPUT = 3,
    BACNET_OBJECT_DEVICE = 8,
    BACNET_SERVICE_I_AM = 0,
    BACNET_SERVICE_READ_PROPERTY = 12,
    BACNET_SERVICE_WHO_IS = 8,
    BACNET_NPDU_NETWORK_MESSAGE = 0x80,
    BACNET_NPDU_DESTINATION_SPECIFIED = 0x20,
    BACNET_NPDU_SOURCE_SPECIFIED = 0x08,
    BACNET_NPDU_RESERVED_BITS = 0x50,
    BACNET_GLOBAL_NETWORK = 0xFFFF,
    BACNET_ERROR_CLASS_OBJECT = 1,
    BACNET_ERROR_CLASS_PROPERTY = 2,
    BACNET_ERROR_UNKNOWN_OBJECT = 31,
    BACNET_ERROR_UNKNOWN_PROPERTY = 32,
    BACNET_ERROR_INVALID_ARRAY_INDEX = 42,
    BACNET_ERROR_PROPERTY_IS_NOT_AN_ARRAY = 50,
};

enum {
    PROP_ACKED_TRANSITIONS = 0,
    PROP_ACTIVE_TEXT = 4,
    PROP_APDU_SEGMENT_TIMEOUT = 10,
    PROP_APDU_TIMEOUT = 11,
    PROP_APPLICATION_SOFTWARE_VERSION = 12,
    PROP_DESCRIPTION = 28,
    PROP_DEVICE_ADDRESS_BINDING = 30,
    PROP_EVENT_STATE = 36,
    PROP_FIRMWARE_REVISION = 44,
    PROP_INACTIVE_TEXT = 46,
    PROP_LOCATION = 58,
    PROP_MAX_APDU_LENGTH_ACCEPTED = 62,
    PROP_MODEL_NAME = 70,
    PROP_NUMBER_OF_APDU_RETRIES = 73,
    PROP_OBJECT_IDENTIFIER = 75,
    PROP_OBJECT_LIST = 76,
    PROP_OBJECT_NAME = 77,
    PROP_OBJECT_TYPE = 79,
    PROP_OUT_OF_SERVICE = 81,
    PROP_POLARITY = 84,
    PROP_PRESENT_VALUE = 85,
    PROP_PROTOCOL_OBJECT_TYPES_SUPPORTED = 96,
    PROP_PROTOCOL_SERVICES_SUPPORTED = 97,
    PROP_PROTOCOL_VERSION = 98,
    PROP_RELIABILITY = 103,
    PROP_SEGMENTATION_SUPPORTED = 107,
    PROP_STATUS_FLAGS = 111,
    PROP_SYSTEM_STATUS = 112,
    PROP_VENDOR_IDENTIFIER = 120,
    PROP_VENDOR_NAME = 121,
    PROP_PROTOCOL_REVISION = 139,
    PROP_DATABASE_REVISION = 155,
    PROP_PROPERTY_LIST = 371,
};

static const uint16_t DEVICE_PROPERTIES[] = {
    PROP_OBJECT_IDENTIFIER,
    PROP_OBJECT_NAME,
    PROP_OBJECT_TYPE,
    PROP_SYSTEM_STATUS,
    PROP_VENDOR_NAME,
    PROP_VENDOR_IDENTIFIER,
    PROP_MODEL_NAME,
    PROP_FIRMWARE_REVISION,
    PROP_APPLICATION_SOFTWARE_VERSION,
    PROP_PROTOCOL_VERSION,
    PROP_PROTOCOL_REVISION,
    PROP_PROTOCOL_SERVICES_SUPPORTED,
    PROP_PROTOCOL_OBJECT_TYPES_SUPPORTED,
    PROP_OBJECT_LIST,
    PROP_MAX_APDU_LENGTH_ACCEPTED,
    PROP_SEGMENTATION_SUPPORTED,
    PROP_APDU_TIMEOUT,
    PROP_NUMBER_OF_APDU_RETRIES,
    PROP_DEVICE_ADDRESS_BINDING,
    PROP_DATABASE_REVISION,
    PROP_DESCRIPTION,
    PROP_LOCATION,
    PROP_PROPERTY_LIST,
};

static const uint16_t BINARY_INPUT_PROPERTIES[] = {
    PROP_OBJECT_IDENTIFIER,
    PROP_OBJECT_NAME,
    PROP_OBJECT_TYPE,
    PROP_PRESENT_VALUE,
    PROP_STATUS_FLAGS,
    PROP_EVENT_STATE,
    PROP_OUT_OF_SERVICE,
    PROP_POLARITY,
    PROP_RELIABILITY,
    PROP_ACTIVE_TEXT,
    PROP_INACTIVE_TEXT,
    PROP_DESCRIPTION,
    PROP_PROPERTY_LIST,
};

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t length;
    bool failed;
} writer_t;

typedef enum {
    NPDU_PARSE_OK,
    NPDU_PARSE_IGNORED,
    NPDU_PARSE_MALFORMED,
} npdu_parse_status_t;

typedef struct {
    const uint8_t *apdu;
    size_t apdu_length;
    bool routed_source;
} application_npdu_t;

typedef struct {
    uint32_t object_type;
    uint32_t object_instance;
    uint32_t property_identifier;
    bool has_array_index;
    uint32_t array_index;
} read_property_request_t;

typedef struct {
    uint8_t tag;
    bool context;
    size_t length;
    size_t header_length;
} decoded_tag_t;

typedef struct {
    bool ok;
    uint8_t error_class;
    uint8_t error_code;
} property_result_t;

static void write_u8(writer_t *writer, uint8_t value)
{
    if (writer->failed || writer->length >= writer->capacity) {
        writer->failed = true;
        return;
    }
    writer->data[writer->length++] = value;
}

static void write_bytes(writer_t *writer, const uint8_t *value, size_t length)
{
    if (writer->failed || length > writer->capacity - writer->length) {
        writer->failed = true;
        return;
    }
    memcpy(writer->data + writer->length, value, length);
    writer->length += length;
}

static void write_be16(writer_t *writer, uint16_t value)
{
    write_u8(writer, (uint8_t)(value >> 8));
    write_u8(writer, (uint8_t)value);
}

static void write_be32(writer_t *writer, uint32_t value)
{
    write_u8(writer, (uint8_t)(value >> 24));
    write_u8(writer, (uint8_t)(value >> 16));
    write_u8(writer, (uint8_t)(value >> 8));
    write_u8(writer, (uint8_t)value);
}

static size_t unsigned_length(uint32_t value)
{
    if (value <= UINT8_MAX) {
        return 1;
    }
    if (value <= UINT16_MAX) {
        return 2;
    }
    if (value <= 0xFFFFFFU) {
        return 3;
    }
    return 4;
}

static void encode_tag(writer_t *writer, uint8_t tag, bool context, size_t length)
{
    const uint8_t class_bit = context ? 0x08U : 0U;

    if (tag >= 15U || length >= 254U) {
        writer->failed = true;
        return;
    }
    if (length <= 4U) {
        write_u8(writer, (uint8_t)((tag << 4) | class_bit | length));
    } else {
        write_u8(writer, (uint8_t)((tag << 4) | class_bit | 5U));
        write_u8(writer, (uint8_t)length);
    }
}

static void encode_unsigned_body(writer_t *writer, uint32_t value, size_t length)
{
    for (size_t offset = length; offset > 0; --offset) {
        write_u8(writer, (uint8_t)(value >> ((offset - 1U) * 8U)));
    }
}

static void encode_context_unsigned(writer_t *writer, uint8_t tag, uint32_t value)
{
    const size_t length = unsigned_length(value);
    encode_tag(writer, tag, true, length);
    encode_unsigned_body(writer, value, length);
}

static void encode_application_unsigned(writer_t *writer, uint32_t value)
{
    const size_t length = unsigned_length(value);
    encode_tag(writer, 2, false, length);
    encode_unsigned_body(writer, value, length);
}

static void encode_application_enumerated(writer_t *writer, uint32_t value)
{
    const size_t length = unsigned_length(value);
    encode_tag(writer, 9, false, length);
    encode_unsigned_body(writer, value, length);
}

static void encode_application_boolean(writer_t *writer, bool value)
{
    write_u8(writer, (uint8_t)(0x10U | (value ? 1U : 0U)));
}

static uint32_t object_identifier(uint32_t object_type, uint32_t instance)
{
    return (object_type << 22) | (instance & BACNET_MAX_INSTANCE);
}

static size_t bounded_string_length(const char *text, size_t maximum)
{
    size_t length = 0;
    while (length < maximum && text[length] != '\0') {
        length++;
    }
    return length;
}

static void encode_context_object_id(
    writer_t *writer,
    uint8_t tag,
    uint32_t object_type,
    uint32_t instance)
{
    encode_tag(writer, tag, true, 4);
    write_be32(writer, object_identifier(object_type, instance));
}

static void encode_application_object_id(
    writer_t *writer,
    uint32_t object_type,
    uint32_t instance)
{
    encode_tag(writer, 12, false, 4);
    write_be32(writer, object_identifier(object_type, instance));
}

static void encode_application_character_string(writer_t *writer, const char *value)
{
    const char *text = value != NULL ? value : "";
    size_t length = bounded_string_length(text, 240U);

    encode_tag(writer, 7, false, length + 1U);
    write_u8(writer, 0); /* ANSI X3.4 / UTF-8 compatible ASCII subset. */
    for (size_t index = 0; index < length; ++index) {
        const uint8_t character = (uint8_t)text[index];
        write_u8(
            writer,
            character >= 0x20U && character <= 0x7EU ? character : (uint8_t)'-');
    }
}

static void encode_application_bit_string(
    writer_t *writer,
    size_t bits_used,
    const uint8_t *set_bits,
    size_t set_bit_count)
{
    const size_t byte_count = (bits_used + 7U) / 8U;
    uint8_t encoded[8] = {0};

    if (byte_count > sizeof(encoded)) {
        writer->failed = true;
        return;
    }
    for (size_t index = 0; index < set_bit_count; ++index) {
        const uint8_t bit = set_bits[index];
        if (bit >= bits_used) {
            writer->failed = true;
            return;
        }
        encoded[bit / 8U] |= (uint8_t)(1U << (7U - bit % 8U));
    }
    encode_tag(writer, 8, false, byte_count + 1U);
    write_u8(writer, (uint8_t)(byte_count * 8U - bits_used));
    write_bytes(writer, encoded, byte_count);
}

static bool decode_tag(const uint8_t *data, size_t length, decoded_tag_t *tag)
{
    if (data == NULL || tag == NULL || length == 0U) {
        return false;
    }
    const uint8_t first = data[0];
    size_t header_length = 1U;
    uint8_t tag_number = first >> 4;

    if (tag_number == 0x0FU) {
        if (length < 2U) {
            return false;
        }
        tag_number = data[1];
        header_length++;
    }

    size_t value_length = first & 0x07U;
    if (value_length == 6U || value_length == 7U) {
        return false;
    }
    if (value_length == 5U) {
        if (length <= header_length || data[header_length] >= 254U) {
            return false;
        }
        value_length = data[header_length];
        header_length++;
    }

    tag->tag = tag_number;
    tag->context = (first & 0x08U) != 0U;
    tag->length = value_length;
    tag->header_length = header_length;
    return header_length + value_length <= length;
}

static bool decode_context_unsigned(
    const uint8_t *data,
    size_t length,
    uint8_t expected_tag,
    uint32_t *value,
    size_t *used)
{
    decoded_tag_t tag;

    if (!decode_tag(data, length, &tag) || tag.tag != expected_tag ||
        !tag.context || tag.length < 1U || tag.length > 4U) {
        return false;
    }

    uint32_t decoded = 0;
    for (size_t index = 0; index < tag.length; ++index) {
        decoded = (decoded << 8) | data[tag.header_length + index];
    }
    *value = decoded;
    *used = tag.header_length + tag.length;
    return true;
}

static bool decode_context_object_id(
    const uint8_t *data,
    size_t length,
    uint32_t *object_type,
    uint32_t *instance,
    size_t *used)
{
    decoded_tag_t tag;

    if (!decode_tag(data, length, &tag) || tag.tag != 0U || !tag.context ||
        tag.length != 4U) {
        return false;
    }
    const uint8_t *value = data + tag.header_length;
    const uint32_t encoded =
        ((uint32_t)value[0] << 24) | ((uint32_t)value[1] << 16) |
        ((uint32_t)value[2] << 8) | value[3];
    *object_type = encoded >> 22;
    *instance = encoded & BACNET_MAX_INSTANCE;
    *used = tag.header_length + tag.length;
    return true;
}

static bool parse_who_is(
    const uint8_t *payload,
    size_t length,
    bool *has_limits,
    uint32_t *low_limit,
    uint32_t *high_limit)
{
    if (length == 0U) {
        *has_limits = false;
        return true;
    }

    size_t low_used;
    size_t high_used;
    if (!decode_context_unsigned(payload, length, 0, low_limit, &low_used) ||
        !decode_context_unsigned(
            payload + low_used,
            length - low_used,
            1,
            high_limit,
            &high_used) ||
        low_used + high_used != length || *low_limit > *high_limit ||
        *low_limit > BACNET_MAX_INSTANCE || *high_limit > BACNET_MAX_INSTANCE) {
        return false;
    }
    *has_limits = true;
    return true;
}

static bool parse_read_property(
    const uint8_t *payload,
    size_t length,
    read_property_request_t *request,
    uint8_t *reject_reason)
{
    size_t object_used;
    size_t property_used;
    size_t consumed;

    if (!decode_context_object_id(
            payload,
            length,
            &request->object_type,
            &request->object_instance,
            &object_used) ||
        !decode_context_unsigned(
            payload + object_used,
            length - object_used,
            1,
            &request->property_identifier,
            &property_used)) {
        *reject_reason = 4; /* missing-required-parameter */
        return false;
    }

    consumed = object_used + property_used;
    request->has_array_index = false;
    request->array_index = 0;
    if (consumed < length) {
        size_t array_used;
        if (!decode_context_unsigned(
                payload + consumed,
                length - consumed,
                2,
                &request->array_index,
                &array_used)) {
            *reject_reason = 4;
            return false;
        }
        request->has_array_index = true;
        consumed += array_used;
    }
    if (consumed != length) {
        *reject_reason = 7; /* too-many-arguments */
        return false;
    }
    return true;
}

static bool start_bvlc(writer_t *writer, bool broadcast)
{
    write_u8(writer, BACNET_BVLC_TYPE);
    write_u8(
        writer,
        broadcast ? BACNET_BVLC_ORIGINAL_BROADCAST_NPDU
                  : BACNET_BVLC_ORIGINAL_UNICAST_NPDU);
    write_be16(writer, 0);
    write_u8(writer, 1); /* BACnet protocol version */
    if (broadcast) {
        write_u8(writer, BACNET_NPDU_DESTINATION_SPECIFIED);
        write_be16(writer, BACNET_GLOBAL_NETWORK);
        write_u8(writer, 0); /* broadcast DADR */
        write_u8(writer, 0xFF); /* hop count */
    } else {
        write_u8(writer, 0); /* local network, normal priority */
    }
    return !writer->failed;
}

static size_t finish_bvlc(writer_t *writer)
{
    if (writer->failed || writer->length > UINT16_MAX) {
        return 0;
    }
    writer->data[2] = (uint8_t)(writer->length >> 8);
    writer->data[3] = (uint8_t)writer->length;
    return writer->length;
}

size_t bacnet_encode_i_am(
    const bacnet_device_state_t *state,
    bool broadcast,
    uint8_t *response,
    size_t response_capacity)
{
    if (state == NULL || response == NULL ||
        state->device_instance >= BACNET_MAX_INSTANCE) {
        return 0;
    }

    writer_t writer = {
        .data = response,
        .capacity = response_capacity,
    };
    start_bvlc(&writer, broadcast);
    write_u8(&writer, 0x10); /* Unconfirmed-Request-PDU */
    write_u8(&writer, BACNET_SERVICE_I_AM);
    encode_application_object_id(
        &writer, BACNET_OBJECT_DEVICE, state->device_instance);
    encode_application_unsigned(&writer, BACNET_MAX_APDU);
    encode_application_enumerated(&writer, 3); /* no-segmentation */
    encode_application_unsigned(&writer, state->vendor_identifier);
    return finish_bvlc(&writer);
}

static property_result_t property_ok(void)
{
    return (property_result_t){.ok = true};
}

static property_result_t property_error(uint8_t error_class, uint8_t error_code)
{
    return (property_result_t){
        .ok = false,
        .error_class = error_class,
        .error_code = error_code,
    };
}

static property_result_t encode_array_item(
    writer_t *writer,
    const bacnet_device_state_t *state,
    bool object_list,
    bool binary_property_list,
    uint32_t index)
{
    if (object_list) {
        if (index == 1U) {
            encode_application_object_id(
                writer, BACNET_OBJECT_DEVICE, state->device_instance);
        } else {
            const size_t input_index = index - 2U;
            encode_application_object_id(
                writer,
                BACNET_OBJECT_BINARY_INPUT,
                state->binary_input_instances[input_index]);
        }
        return property_ok();
    }

    const uint16_t *properties =
        binary_property_list ? BINARY_INPUT_PROPERTIES : DEVICE_PROPERTIES;
    encode_application_enumerated(writer, properties[index - 1U]);
    return property_ok();
}

static property_result_t encode_array(
    writer_t *writer,
    const bacnet_device_state_t *state,
    const read_property_request_t *request,
    bool object_list,
    bool binary_property_list)
{
    const uint32_t count = object_list
        ? 1U + BACNET_BINARY_INPUT_COUNT
        : (uint32_t)(binary_property_list
                  ? sizeof(BINARY_INPUT_PROPERTIES) /
                        sizeof(BINARY_INPUT_PROPERTIES[0])
                  : sizeof(DEVICE_PROPERTIES) / sizeof(DEVICE_PROPERTIES[0]));

    if (request->has_array_index && request->array_index == 0U) {
        encode_application_unsigned(writer, count);
        return property_ok();
    }
    if (request->has_array_index) {
        if (request->array_index > count) {
            return property_error(
                BACNET_ERROR_CLASS_PROPERTY, BACNET_ERROR_INVALID_ARRAY_INDEX);
        }
        return encode_array_item(
            writer,
            state,
            object_list,
            binary_property_list,
            request->array_index);
    }
    for (uint32_t index = 1; index <= count; ++index) {
        encode_array_item(
            writer, state, object_list, binary_property_list, index);
    }
    return property_ok();
}

static property_result_t encode_device_property(
    writer_t *writer,
    const bacnet_device_state_t *state,
    const read_property_request_t *request)
{
    const uint32_t property = request->property_identifier;
    const bool is_array =
        property == PROP_OBJECT_LIST || property == PROP_PROPERTY_LIST;

    if (request->has_array_index && !is_array) {
        return property_error(
            BACNET_ERROR_CLASS_PROPERTY,
            BACNET_ERROR_PROPERTY_IS_NOT_AN_ARRAY);
    }

    switch (property) {
    case PROP_OBJECT_IDENTIFIER:
        encode_application_object_id(
            writer, BACNET_OBJECT_DEVICE, state->device_instance);
        break;
    case PROP_OBJECT_NAME:
        encode_application_character_string(writer, state->device_name);
        break;
    case PROP_OBJECT_TYPE:
        encode_application_enumerated(writer, BACNET_OBJECT_DEVICE);
        break;
    case PROP_SYSTEM_STATUS:
        encode_application_enumerated(writer, 1); /* operational-read-only */
        break;
    case PROP_VENDOR_NAME:
        encode_application_character_string(writer, state->vendor_name);
        break;
    case PROP_VENDOR_IDENTIFIER:
        encode_application_unsigned(writer, state->vendor_identifier);
        break;
    case PROP_MODEL_NAME:
        encode_application_character_string(writer, state->model_name);
        break;
    case PROP_FIRMWARE_REVISION:
    case PROP_APPLICATION_SOFTWARE_VERSION:
        encode_application_character_string(writer, state->firmware_revision);
        break;
    case PROP_PROTOCOL_VERSION:
        encode_application_unsigned(writer, 1);
        break;
    case PROP_PROTOCOL_REVISION:
        encode_application_unsigned(writer, 14);
        break;
    case PROP_PROTOCOL_SERVICES_SUPPORTED: {
        const uint8_t set_bits[] = {12, 26, 34};
        encode_application_bit_string(
            writer, 35, set_bits, sizeof(set_bits));
        break;
    }
    case PROP_PROTOCOL_OBJECT_TYPES_SUPPORTED: {
        const uint8_t set_bits[] = {
            BACNET_OBJECT_BINARY_INPUT,
            BACNET_OBJECT_DEVICE,
        };
        encode_application_bit_string(
            writer, 9, set_bits, sizeof(set_bits));
        break;
    }
    case PROP_OBJECT_LIST:
        return encode_array(writer, state, request, true, false);
    case PROP_MAX_APDU_LENGTH_ACCEPTED:
        encode_application_unsigned(writer, BACNET_MAX_APDU);
        break;
    case PROP_SEGMENTATION_SUPPORTED:
        encode_application_enumerated(writer, 3); /* no-segmentation */
        break;
    case PROP_APDU_TIMEOUT:
        encode_application_unsigned(writer, 3000);
        break;
    case PROP_NUMBER_OF_APDU_RETRIES:
        encode_application_unsigned(writer, 3);
        break;
    case PROP_DEVICE_ADDRESS_BINDING:
        /* Empty BACnetARRAY of BACnetAddressBinding. */
        break;
    case PROP_DATABASE_REVISION:
        encode_application_unsigned(writer, state->database_revision);
        break;
    case PROP_DESCRIPTION:
        encode_application_character_string(writer, state->description);
        break;
    case PROP_LOCATION:
        encode_application_character_string(writer, "");
        break;
    case PROP_PROPERTY_LIST:
        return encode_array(writer, state, request, false, false);
    default:
        return property_error(
            BACNET_ERROR_CLASS_PROPERTY, BACNET_ERROR_UNKNOWN_PROPERTY);
    }
    return property_ok();
}

static property_result_t encode_binary_input_property(
    writer_t *writer,
    const bacnet_device_state_t *state,
    const read_property_request_t *request,
    size_t input_index)
{
    const uint32_t property = request->property_identifier;

    if (request->has_array_index && property != PROP_PROPERTY_LIST) {
        return property_error(
            BACNET_ERROR_CLASS_PROPERTY,
            BACNET_ERROR_PROPERTY_IS_NOT_AN_ARRAY);
    }

    switch (property) {
    case PROP_OBJECT_IDENTIFIER:
        encode_application_object_id(
            writer,
            BACNET_OBJECT_BINARY_INPUT,
            state->binary_input_instances[input_index]);
        break;
    case PROP_OBJECT_NAME:
        encode_application_character_string(
            writer, state->binary_input_names[input_index]);
        break;
    case PROP_OBJECT_TYPE:
        encode_application_enumerated(writer, BACNET_OBJECT_BINARY_INPUT);
        break;
    case PROP_PRESENT_VALUE:
        encode_application_enumerated(
            writer, state->binary_input_values[input_index] ? 1U : 0U);
        break;
    case PROP_STATUS_FLAGS:
        encode_application_bit_string(writer, 4, NULL, 0);
        break;
    case PROP_EVENT_STATE:
    case PROP_RELIABILITY:
    case PROP_POLARITY:
        encode_application_enumerated(writer, 0);
        break;
    case PROP_OUT_OF_SERVICE:
        encode_application_boolean(writer, false);
        break;
    case PROP_ACTIVE_TEXT:
        encode_application_character_string(writer, "Active");
        break;
    case PROP_INACTIVE_TEXT:
        encode_application_character_string(writer, "Inactive");
        break;
    case PROP_DESCRIPTION:
        encode_application_character_string(
            writer, state->binary_input_descriptions[input_index]);
        break;
    case PROP_PROPERTY_LIST:
        return encode_array(writer, state, request, false, true);
    default:
        return property_error(
            BACNET_ERROR_CLASS_PROPERTY, BACNET_ERROR_UNKNOWN_PROPERTY);
    }
    return property_ok();
}

static bool find_binary_input(
    const bacnet_device_state_t *state,
    uint32_t instance,
    size_t *input_index)
{
    for (size_t index = 0; index < BACNET_BINARY_INPUT_COUNT; ++index) {
        if (state->binary_input_instances[index] == instance) {
            *input_index = index;
            return true;
        }
    }
    return false;
}

static size_t encode_wrapped_short_apdu(
    uint8_t first,
    uint8_t invoke_id,
    uint8_t third,
    uint8_t *response,
    size_t capacity)
{
    writer_t writer = {.data = response, .capacity = capacity};
    start_bvlc(&writer, false);
    write_u8(&writer, first);
    write_u8(&writer, invoke_id);
    write_u8(&writer, third);
    return finish_bvlc(&writer);
}

static size_t encode_error(
    uint8_t invoke_id,
    uint8_t error_class,
    uint8_t error_code,
    uint8_t *response,
    size_t capacity)
{
    writer_t writer = {.data = response, .capacity = capacity};
    start_bvlc(&writer, false);
    write_u8(&writer, 0x50); /* Error-PDU */
    write_u8(&writer, invoke_id);
    write_u8(&writer, BACNET_SERVICE_READ_PROPERTY);
    encode_application_enumerated(&writer, error_class);
    encode_application_enumerated(&writer, error_code);
    return finish_bvlc(&writer);
}

static size_t encode_read_property_response(
    uint8_t invoke_id,
    const read_property_request_t *request,
    const bacnet_device_state_t *state,
    uint8_t *response,
    size_t response_capacity)
{
    uint8_t value[BACNET_MAX_APDU];
    writer_t value_writer = {.data = value, .capacity = sizeof(value)};
    property_result_t result;

    if (request->object_type == BACNET_OBJECT_DEVICE &&
        request->object_instance == state->device_instance) {
        result = encode_device_property(&value_writer, state, request);
    } else if (request->object_type == BACNET_OBJECT_BINARY_INPUT) {
        size_t input_index;
        if (!find_binary_input(state, request->object_instance, &input_index)) {
            result = property_error(
                BACNET_ERROR_CLASS_OBJECT, BACNET_ERROR_UNKNOWN_OBJECT);
        } else {
            result = encode_binary_input_property(
                &value_writer, state, request, input_index);
        }
    } else {
        result = property_error(
            BACNET_ERROR_CLASS_OBJECT, BACNET_ERROR_UNKNOWN_OBJECT);
    }

    if (!result.ok) {
        return encode_error(
            invoke_id,
            result.error_class,
            result.error_code,
            response,
            response_capacity);
    }
    if (value_writer.failed) {
        return encode_wrapped_short_apdu(
            0x71, invoke_id, 4, response, response_capacity);
    }

    writer_t writer = {.data = response, .capacity = response_capacity};
    start_bvlc(&writer, false);
    write_u8(&writer, 0x30); /* Complex-Ack-PDU */
    write_u8(&writer, invoke_id);
    write_u8(&writer, BACNET_SERVICE_READ_PROPERTY);
    encode_context_object_id(
        &writer,
        0,
        request->object_type,
        request->object_instance);
    encode_context_unsigned(&writer, 1, request->property_identifier);
    if (request->has_array_index) {
        encode_context_unsigned(&writer, 2, request->array_index);
    }
    write_u8(&writer, 0x3E); /* opening tag 3 */
    write_bytes(&writer, value, value_writer.length);
    write_u8(&writer, 0x3F); /* closing tag 3 */

    if (writer.length > BACNET_MAX_APDU + 6U) {
        return encode_wrapped_short_apdu(
            0x71, invoke_id, 4, response, response_capacity);
    }
    return finish_bvlc(&writer);
}

static npdu_parse_status_t parse_application_npdu(
    const uint8_t *frame,
    size_t frame_length,
    application_npdu_t *npdu)
{
    const uint8_t control = frame[5];
    size_t offset = 6U;
    uint16_t destination_network = 0;
    uint8_t destination_length = 0;

    if ((control & BACNET_NPDU_RESERVED_BITS) != 0U) {
        return NPDU_PARSE_MALFORMED;
    }
    if ((control & BACNET_NPDU_NETWORK_MESSAGE) != 0U) {
        return NPDU_PARSE_IGNORED;
    }

    if ((control & BACNET_NPDU_DESTINATION_SPECIFIED) != 0U) {
        if (frame_length - offset < 3U) {
            return NPDU_PARSE_MALFORMED;
        }
        destination_network =
            (uint16_t)(((uint16_t)frame[offset] << 8) | frame[offset + 1U]);
        destination_length = frame[offset + 2U];
        offset += 3U;
        if (destination_network == 0U ||
            (destination_network == BACNET_GLOBAL_NETWORK &&
             destination_length != 0U) ||
            (size_t)destination_length > frame_length - offset) {
            return NPDU_PARSE_MALFORMED;
        }
        offset += destination_length;
    }

    npdu->routed_source =
        (control & BACNET_NPDU_SOURCE_SPECIFIED) != 0U;
    if (npdu->routed_source) {
        if (frame_length - offset < 3U) {
            return NPDU_PARSE_MALFORMED;
        }
        const uint16_t source_network =
            (uint16_t)(((uint16_t)frame[offset] << 8) | frame[offset + 1U]);
        const uint8_t source_length = frame[offset + 2U];
        offset += 3U;
        if (source_network == 0U || source_network == BACNET_GLOBAL_NETWORK ||
            source_length == 0U ||
            (size_t)source_length > frame_length - offset) {
            return NPDU_PARSE_MALFORMED;
        }
        offset += source_length;
    }

    if ((control & BACNET_NPDU_DESTINATION_SPECIFIED) != 0U) {
        if (offset >= frame_length) {
            return NPDU_PARSE_MALFORMED;
        }
        offset++; /* hop count */
        if (destination_network != BACNET_GLOBAL_NETWORK) {
            return NPDU_PARSE_IGNORED;
        }
    }
    if (offset >= frame_length) {
        return NPDU_PARSE_MALFORMED;
    }

    npdu->apdu = frame + offset;
    npdu->apdu_length = frame_length - offset;
    return NPDU_PARSE_OK;
}

bacnet_packet_result_t bacnet_handle_packet(
    const uint8_t *frame,
    size_t frame_length,
    const bacnet_device_state_t *state,
    uint8_t *response,
    size_t response_capacity)
{
    bacnet_packet_result_t result = {
        .kind = BACNET_PACKET_MALFORMED,
        .response_length = 0,
        .broadcast_response = false,
    };

    if (frame == NULL || state == NULL || response == NULL || frame_length < 8U ||
        frame_length > BACNET_MAX_REQUEST_BYTES || frame[0] != BACNET_BVLC_TYPE ||
        (frame[1] != BACNET_BVLC_ORIGINAL_UNICAST_NPDU &&
         frame[1] != BACNET_BVLC_ORIGINAL_BROADCAST_NPDU) ||
        (((size_t)frame[2] << 8) | frame[3]) != frame_length || frame[4] != 1U) {
        return result;
    }

    application_npdu_t npdu;
    const npdu_parse_status_t npdu_status =
        parse_application_npdu(frame, frame_length, &npdu);
    if (npdu_status == NPDU_PARSE_IGNORED) {
        result.kind = BACNET_PACKET_IGNORED;
        return result;
    }
    if (npdu_status == NPDU_PARSE_MALFORMED) {
        return result;
    }

    const uint8_t *apdu = npdu.apdu;
    const size_t apdu_length = npdu.apdu_length;
    if (apdu_length >= 2U && apdu[0] == 0x10U &&
        apdu[1] == BACNET_SERVICE_WHO_IS) {
        bool has_limits;
        uint32_t low_limit = 0;
        uint32_t high_limit = 0;
        result.kind = BACNET_PACKET_WHO_IS;
        if (!parse_who_is(
                apdu + 2,
                apdu_length - 2U,
                &has_limits,
                &low_limit,
                &high_limit)) {
            result.kind = BACNET_PACKET_MALFORMED;
            return result;
        }
        if (!has_limits ||
            (low_limit <= state->device_instance &&
             state->device_instance <= high_limit)) {
            result.broadcast_response = true;
            result.response_length = bacnet_encode_i_am(
                state, true, response, response_capacity);
        }
        return result;
    }

    if (apdu_length == 0U || (apdu[0] & 0xF0U) != 0U) {
        result.kind = BACNET_PACKET_IGNORED;
        return result;
    }
    if (npdu.routed_source) {
        result.kind = BACNET_PACKET_IGNORED;
        return result;
    }
    if (apdu_length < 4U) {
        return result;
    }

    const uint8_t invoke_id = apdu[2];
    result.kind = BACNET_PACKET_READ_PROPERTY;
    if ((apdu[0] & 0x0CU) != 0U) {
        result.response_length = encode_wrapped_short_apdu(
            0x71, invoke_id, 4, response, response_capacity);
        return result;
    }
    if (apdu[3] != BACNET_SERVICE_READ_PROPERTY) {
        result.response_length = encode_wrapped_short_apdu(
            0x60, invoke_id, 9, response, response_capacity);
        return result;
    }

    read_property_request_t request;
    uint8_t reject_reason = 4;
    if (!parse_read_property(
            apdu + 4, apdu_length - 4U, &request, &reject_reason)) {
        result.response_length = encode_wrapped_short_apdu(
            0x60, invoke_id, reject_reason, response, response_capacity);
        return result;
    }
    result.response_length = encode_read_property_response(
        invoke_id, &request, state, response, response_capacity);
    return result;
}
