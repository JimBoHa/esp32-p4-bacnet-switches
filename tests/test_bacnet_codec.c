#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bacnet_codec.h"
#include "config_model.h"
#include "cov_retry_cache.h"
#include "diagnostics_time.h"
#include "diagnostics_metrics.h"
#include "hardware_profile.h"
#include "input_line_classifier.h"
#include "input_debounce.h"
#include "network_config_model.h"
#include "ota_auth.h"
#include "ota_health.h"

#define ARRAY_LENGTH(value) (sizeof(value) / sizeof((value)[0]))

static unsigned tests_run;

static void fail(const char *expression, const char *file, int line)
{
    fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
    exit(EXIT_FAILURE);
}

#define CHECK(expression)                                                       \
    do {                                                                        \
        tests_run++;                                                            \
        if (!(expression)) {                                                    \
            fail(#expression, __FILE__, __LINE__);                              \
        }                                                                       \
    } while (0)

static const bacnet_device_state_t STATE = {
    .device_instance = 599001,
    .vendor_identifier = 999,
    .device_name = "ESP32-P4 Toggle Inputs",
    .vendor_name = "Lab placeholder",
    .model_name = "Waveshare ESP32-P4-POE-ETH",
    .firmware_revision = "1.3.2",
    .application_software_version = "1.3.2 (0123456789ab)",
    .description = "Three toggle inputs",
    .location = "Test bench",
    .database_revision = 3,
    .binary_input_instances = {20, 21, 22, 1001, 1002},
    .binary_input_names = {
        "GPIO20 Toggle",
        "GPIO21 Toggle",
        "GPIO22 Toggle",
        "Status Ethernet Link",
        "Status IPv4 Assigned",
    },
    .binary_input_descriptions = {
        "GPIO20 input",
        "GPIO21 input",
        "GPIO22 input",
        "Physical Ethernet link is up",
        "Ethernet interface has an IPv4 address",
    },
    .binary_input_values = {false, true, false, true, true},
    .binary_input_reliability = {7, 0, 0, 0, 0},
    .binary_input_active_low = {false, true, false, false, false},
    .analog_value_instances = {
        1000, 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009, 1010,
    },
    .analog_value_names = {
        "Chip Temperature",
        "System Uptime",
        "Free Heap",
        "Minimum Free Heap",
        "Ethernet Link Losses",
        "Ethernet Reconnects",
        "BACnet RX Packets",
        "BACnet Protocol Errors",
        "Last Reset Reason",
        "Active COV Subscriptions",
        "Boot Count",
    },
    .analog_value_descriptions = {
        "Internal die temperature",
        "Seconds since boot",
        "Free heap bytes",
        "Minimum free heap bytes",
        "Link-down count",
        "Reconnect count",
        "Received datagrams",
        "Protocol error count",
        "Reset reason code",
        "Active subscriptions",
        "Persistent boot counter",
    },
    .analog_value_values = {
        42.5F, 123.0F, 500000.0F, 450000.0F, 1.0F,
        2.0F, 300.0F, 4.0F, 1.0F, 2.0F, 31.0F,
    },
    .analog_value_units = {62, 73, 95, 95, 95, 95, 95, 95, 95, 95, 95},
    .analog_value_reliability = {0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0},
    .network_port_instance = 1U,
    .network_port_name = "BACnet/IP Ethernet",
    .network_port_description = "Primary BACnet/IPv4 Ethernet interface",
    .network_port_reliability = 0U,
    .network_port_out_of_service = false,
    .network_port_changes_pending = true,
    .network_port_link_speed_bps = 100000000.0F,
    .network_port_udp_port = 47808U,
    .network_port_dhcp_enabled = true,
    .network_port_ipv4 = {192U, 168U, 75U, 152U},
    .network_port_netmask = {255U, 255U, 255U, 0U},
    .network_port_gateway = {192U, 168U, 75U, 1U},
    .network_port_dns = {
        {192U, 168U, 75U, 1U},
        {8U, 8U, 8U, 8U},
        {1U, 1U, 1U, 1U},
    },
};

static bool bytes_equal(
    const uint8_t *actual,
    size_t actual_length,
    const uint8_t *expected,
    size_t expected_length)
{
    return actual_length == expected_length &&
        memcmp(actual, expected, expected_length) == 0;
}

static bool contains_bytes(
    const uint8_t *haystack,
    size_t haystack_length,
    const uint8_t *needle,
    size_t needle_length)
{
    if (needle_length > haystack_length) {
        return false;
    }
    for (size_t index = 0; index <= haystack_length - needle_length; ++index) {
        if (memcmp(haystack + index, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
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

static void append_context_unsigned(
    uint8_t *frame,
    size_t *length,
    uint8_t tag,
    uint32_t value)
{
    const size_t encoded_length = unsigned_length(value);
    frame[(*length)++] = (uint8_t)((tag << 4) | 0x08U | encoded_length);
    for (size_t offset = encoded_length; offset > 0; --offset) {
        frame[(*length)++] =
            (uint8_t)(value >> ((offset - 1U) * 8U));
    }
}

static void append_context_object_id(
    uint8_t *frame,
    size_t *length,
    uint8_t tag,
    uint32_t object_type,
    uint32_t object_instance)
{
    const uint32_t object_id =
        (object_type << 22) | (object_instance & BACNET_MAX_INSTANCE);
    frame[(*length)++] = (uint8_t)((tag << 4) | 0x0CU);
    frame[(*length)++] = (uint8_t)(object_id >> 24);
    frame[(*length)++] = (uint8_t)(object_id >> 16);
    frame[(*length)++] = (uint8_t)(object_id >> 8);
    frame[(*length)++] = (uint8_t)object_id;
}

static size_t begin_confirmed_request(
    uint8_t *frame,
    uint8_t invoke_id,
    uint8_t service)
{
    size_t length = 0U;
    frame[length++] = 0x81;
    frame[length++] = 0x0A;
    frame[length++] = 0U;
    frame[length++] = 0U;
    frame[length++] = 0x01;
    frame[length++] = 0x04;
    frame[length++] = 0x00;
    frame[length++] = 0x05;
    frame[length++] = invoke_id;
    frame[length++] = service;
    return length;
}

static size_t finish_request(uint8_t *frame, size_t length)
{
    frame[2] = (uint8_t)(length >> 8);
    frame[3] = (uint8_t)length;
    return length;
}

static size_t read_property_request(
    uint8_t *frame,
    uint32_t object_type,
    uint32_t object_instance,
    uint32_t property,
    bool has_array_index,
    uint32_t array_index)
{
    size_t length = begin_confirmed_request(frame, 1U, 12U);
    append_context_object_id(
        frame, &length, 0U, object_type, object_instance);
    append_context_unsigned(frame, &length, 1, property);
    if (has_array_index) {
        append_context_unsigned(frame, &length, 2, array_index);
    }
    return finish_request(frame, length);
}

static void check_read_property_tail(
    const bacnet_device_state_t *state,
    uint32_t object_type,
    uint32_t object_instance,
    uint32_t property,
    bool has_array_index,
    uint32_t array_index,
    const uint8_t *expected,
    size_t expected_length)
{
    uint8_t request[BACNET_MAX_REQUEST_BYTES];
    uint8_t response[1500];
    const size_t request_length = read_property_request(
        request,
        object_type,
        object_instance,
        property,
        has_array_index,
        array_index);
    const bacnet_packet_result_t result = bacnet_handle_packet(
        request,
        request_length,
        state,
        response,
        sizeof(response));
    CHECK(result.kind == BACNET_PACKET_READ_PROPERTY);
    CHECK(result.response_length >= expected_length);
    CHECK(memcmp(
        response + result.response_length - expected_length,
        expected,
        expected_length) == 0);
}

static void append_context_character_string(
    uint8_t *frame,
    size_t *length,
    uint8_t tag,
    const char *value)
{
    const size_t text_length = strlen(value);
    const size_t encoded_length = text_length + 1U;
    CHECK(encoded_length < 254U);
    if (encoded_length <= 4U) {
        frame[(*length)++] =
            (uint8_t)((tag << 4U) | 0x08U | encoded_length);
    } else {
        frame[(*length)++] = (uint8_t)((tag << 4U) | 0x08U | 5U);
        frame[(*length)++] = (uint8_t)encoded_length;
    }
    frame[(*length)++] = 0U;
    memcpy(frame + *length, value, text_length);
    *length += text_length;
}

static size_t who_has_request(
    uint8_t *frame,
    uint8_t bvlc_function,
    bool routed_global,
    bool has_limits,
    uint32_t low_limit,
    uint32_t high_limit,
    bool by_name,
    uint32_t object_type,
    uint32_t object_instance,
    const char *object_name)
{
    size_t length = 0U;
    frame[length++] = 0x81U;
    frame[length++] = bvlc_function;
    frame[length++] = 0U;
    frame[length++] = 0U;
    frame[length++] = 0x01U;
    if (routed_global) {
        frame[length++] = 0x20U;
        frame[length++] = 0xFFU;
        frame[length++] = 0xFFU;
        frame[length++] = 0U;
        frame[length++] = 0xFFU;
    } else {
        frame[length++] = 0U;
    }
    frame[length++] = 0x10U;
    frame[length++] = 7U;
    if (has_limits) {
        append_context_unsigned(frame, &length, 0U, low_limit);
        append_context_unsigned(frame, &length, 1U, high_limit);
    }
    if (by_name) {
        append_context_character_string(
            frame, &length, 3U, object_name);
    } else {
        append_context_object_id(
            frame, &length, 2U, object_type, object_instance);
    }
    return finish_request(frame, length);
}

typedef struct {
    uint32_t property;
    bool has_array_index;
    uint32_t array_index;
} test_property_reference_t;

static void append_rpm_object(
    uint8_t *frame,
    size_t *length,
    uint32_t object_type,
    uint32_t object_instance,
    const test_property_reference_t *references,
    size_t reference_count)
{
    append_context_object_id(
        frame, length, 0U, object_type, object_instance);
    frame[(*length)++] = 0x1E;
    for (size_t index = 0; index < reference_count; ++index) {
        append_context_unsigned(
            frame, length, 0U, references[index].property);
        if (references[index].has_array_index) {
            append_context_unsigned(
                frame, length, 1U, references[index].array_index);
        }
    }
    frame[(*length)++] = 0x1F;
}

static size_t subscribe_cov_request(
    uint8_t *frame,
    uint8_t invoke_id,
    uint32_t process_id,
    uint32_t object_type,
    uint32_t object_instance,
    bool include_parameters,
    bool confirmed,
    uint32_t lifetime_seconds)
{
    size_t length = begin_confirmed_request(frame, invoke_id, 5U);
    append_context_unsigned(frame, &length, 0U, process_id);
    append_context_object_id(
        frame, &length, 1U, object_type, object_instance);
    if (include_parameters) {
        append_context_unsigned(frame, &length, 2U, confirmed ? 1U : 0U);
        append_context_unsigned(frame, &length, 3U, lifetime_seconds);
    }
    return finish_request(frame, length);
}

static void test_reference_vectors(void)
{
    static const uint8_t who_is_all[] = {
        0x81, 0x0B, 0x00, 0x08, 0x01, 0x00, 0x10, 0x08,
    };
    static const uint8_t who_is_exact[] = {
        0x81, 0x0A, 0x00, 0x10, 0x01, 0x00, 0x10, 0x08,
        0x0B, 0x09, 0x23, 0xD9, 0x1B, 0x09, 0x23, 0xD9,
    };
    /* Captured from a Metasys server during BACnet/IP discovery. */
    static const uint8_t metasys_global_who_is[] = {
        0x81, 0x0B, 0x00, 0x0C, 0x01, 0x20, 0xFF, 0xFF,
        0x00, 0xFF, 0x10, 0x08,
    };
    static const uint8_t unicast_global_who_is[] = {
        0x81, 0x0A, 0x00, 0x0C, 0x01, 0x20, 0xFF, 0xFF,
        0x00, 0xFF, 0x10, 0x08,
    };
    static const uint8_t metasys_filtered_who_is[] = {
        0x81, 0x0B, 0x00, 0x14, 0x01, 0x20, 0xFF, 0xFF,
        0x00, 0xFF, 0x10, 0x08, 0x0B, 0x1E, 0x84, 0xCB,
        0x1B, 0x1E, 0x84, 0xCB,
    };
    static const uint8_t routed_global_who_is[] = {
        0x81, 0x0B, 0x00, 0x10, 0x01, 0x28, 0xFF, 0xFF,
        0x00, 0x12, 0x34, 0x01, 0xAA, 0xFF, 0x10, 0x08,
    };
    static const uint8_t expected_i_am[] = {
        0x81, 0x0A, 0x00, 0x15, 0x01, 0x00, 0x10, 0x00,
        0xC4, 0x02, 0x09, 0x23, 0xD9, 0x22, 0x05, 0xC4,
        0x91, 0x03, 0x22, 0x03, 0xE7,
    };
    static const uint8_t expected_broadcast_i_am[] = {
        0x81, 0x0B, 0x00, 0x19, 0x01, 0x20, 0xFF, 0xFF,
        0x00, 0xFF, 0x10, 0x00, 0xC4, 0x02, 0x09, 0x23,
        0xD9, 0x22, 0x05, 0xC4, 0x91, 0x03, 0x22, 0x03,
        0xE7,
    };
    uint8_t response[1500];

    size_t length = bacnet_encode_i_am(
        &STATE, false, response, sizeof(response));
    CHECK(bytes_equal(
        response, length, expected_i_am, ARRAY_LENGTH(expected_i_am)));
    length = bacnet_encode_i_am(&STATE, true, response, sizeof(response));
    CHECK(bytes_equal(
        response,
        length,
        expected_broadcast_i_am,
        ARRAY_LENGTH(expected_broadcast_i_am)));

    bacnet_packet_result_t result = bacnet_handle_packet(
        who_is_all,
        sizeof(who_is_all),
        &STATE,
        response,
        sizeof(response));
    CHECK(result.kind == BACNET_PACKET_WHO_IS);
    CHECK(result.broadcast_response);
    CHECK(bytes_equal(
        response,
        result.response_length,
        expected_broadcast_i_am,
        ARRAY_LENGTH(expected_broadcast_i_am)));

    result = bacnet_handle_packet(
        who_is_exact,
        sizeof(who_is_exact),
        &STATE,
        response,
        sizeof(response));
    CHECK(result.kind == BACNET_PACKET_WHO_IS);
    CHECK(!result.broadcast_response);
    CHECK(bytes_equal(
        response,
        result.response_length,
        expected_i_am,
        ARRAY_LENGTH(expected_i_am)));

    result = bacnet_handle_packet(
        metasys_global_who_is,
        sizeof(metasys_global_who_is),
        &STATE,
        response,
        sizeof(response));
    CHECK(result.kind == BACNET_PACKET_WHO_IS);
    CHECK(result.broadcast_response);
    CHECK(bytes_equal(
        response,
        result.response_length,
        expected_broadcast_i_am,
        ARRAY_LENGTH(expected_broadcast_i_am)));

    result = bacnet_handle_packet(
        unicast_global_who_is,
        sizeof(unicast_global_who_is),
        &STATE,
        response,
        sizeof(response));
    CHECK(result.kind == BACNET_PACKET_WHO_IS);
    CHECK(result.broadcast_response);
    CHECK(bytes_equal(
        response,
        result.response_length,
        expected_broadcast_i_am,
        ARRAY_LENGTH(expected_broadcast_i_am)));

    result = bacnet_handle_packet(
        routed_global_who_is,
        sizeof(routed_global_who_is),
        &STATE,
        response,
        sizeof(response));
    CHECK(result.kind == BACNET_PACKET_WHO_IS);
    CHECK(result.broadcast_response);
    CHECK(bytes_equal(
        response,
        result.response_length,
        expected_broadcast_i_am,
        ARRAY_LENGTH(expected_broadcast_i_am)));

    result = bacnet_handle_packet(
        metasys_filtered_who_is,
        sizeof(metasys_filtered_who_is),
        &STATE,
        response,
        sizeof(response));
    CHECK(result.kind == BACNET_PACKET_WHO_IS);
    CHECK(result.response_length == 0);
    CHECK(!result.broadcast_response);

    uint8_t outside[sizeof(who_is_exact)];
    memcpy(outside, who_is_exact, sizeof(outside));
    outside[11]++;
    outside[15]++;
    result = bacnet_handle_packet(
        outside, sizeof(outside), &STATE, response, sizeof(response));
    CHECK(result.kind == BACNET_PACKET_WHO_IS);
    CHECK(result.response_length == 0);
    CHECK(!result.broadcast_response);
}

static void test_who_has_i_have(void)
{
    uint8_t request[BACNET_MAX_REQUEST_BYTES];
    uint8_t response[1500];
    size_t length = who_has_request(
        request,
        0x0AU,
        false,
        false,
        0U,
        0U,
        false,
        3U,
        20U,
        NULL);
    bacnet_packet_result_t result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    CHECK(result.kind == BACNET_PACKET_WHO_HAS);
    CHECK(result.response_length > 20U);
    CHECK(!result.broadcast_response);
    CHECK(response[1] == 0x0AU && response[6] == 0x10U && response[7] == 1U);
    const uint32_t device_id = (8U << 22U) | STATE.device_instance;
    const uint32_t input_id = (3U << 22U) | 20U;
    const uint8_t encoded_device[] = {
        0xC4U,
        (uint8_t)(device_id >> 24U),
        (uint8_t)(device_id >> 16U),
        (uint8_t)(device_id >> 8U),
        (uint8_t)device_id,
    };
    const uint8_t encoded_input[] = {
        0xC4U,
        (uint8_t)(input_id >> 24U),
        (uint8_t)(input_id >> 16U),
        (uint8_t)(input_id >> 8U),
        (uint8_t)input_id,
    };
    CHECK(contains_bytes(
        response,
        result.response_length,
        encoded_device,
        sizeof(encoded_device)));
    CHECK(contains_bytes(
        response,
        result.response_length,
        encoded_input,
        sizeof(encoded_input)));
    CHECK(contains_bytes(
        response,
        result.response_length,
        (const uint8_t *)"GPIO20 Toggle",
        strlen("GPIO20 Toggle")));

    length = who_has_request(
        request,
        0x0BU,
        false,
        true,
        STATE.device_instance,
        STATE.device_instance,
        true,
        0U,
        0U,
        "Chip Temperature");
    result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    CHECK(result.kind == BACNET_PACKET_WHO_HAS);
    CHECK(result.response_length > 24U);
    CHECK(result.broadcast_response);
    CHECK(response[1] == 0x0BU && response[10] == 0x10U && response[11] == 1U);
    CHECK(contains_bytes(
        response,
        result.response_length,
        (const uint8_t *)"Chip Temperature",
        strlen("Chip Temperature")));

    length = who_has_request(
        request,
        0x0BU,
        true,
        false,
        0U,
        0U,
        true,
        0U,
        0U,
        STATE.device_name);
    result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    CHECK(result.kind == BACNET_PACKET_WHO_HAS);
    CHECK(result.response_length > 24U);
    CHECK(result.broadcast_response);
    CHECK(contains_bytes(
        response,
        result.response_length,
        (const uint8_t *)STATE.device_name,
        strlen(STATE.device_name)));

    length = who_has_request(
        request,
        0x0AU,
        false,
        true,
        STATE.device_instance + 1U,
        STATE.device_instance + 10U,
        false,
        3U,
        20U,
        NULL);
    result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    CHECK(result.kind == BACNET_PACKET_WHO_HAS);
    CHECK(result.response_length == 0U);

    length = who_has_request(
        request,
        0x0AU,
        false,
        false,
        0U,
        0U,
        true,
        0U,
        0U,
        "Unknown Object");
    result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    CHECK(result.kind == BACNET_PACKET_WHO_HAS);
    CHECK(result.response_length == 0U);

    length = who_has_request(
        request,
        0x0AU,
        false,
        false,
        0U,
        0U,
        true,
        0U,
        0U,
        "GPIO21 Toggle");
    result = bacnet_handle_packet(
        request, length, &STATE, response, 12U);
    CHECK(result.kind == BACNET_PACKET_WHO_HAS);
    CHECK(result.response_length == 0U);

    request[length++] = 0U;
    length = finish_request(request, length);
    result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    CHECK(result.kind == BACNET_PACKET_MALFORMED);
    CHECK(result.response_length == 0U);

    static const uint8_t missing_object[] = {
        0x81U, 0x0AU, 0x00U, 0x08U, 0x01U, 0x00U, 0x10U, 0x07U,
    };
    result = bacnet_handle_packet(
        missing_object,
        sizeof(missing_object),
        &STATE,
        response,
        sizeof(response));
    CHECK(result.kind == BACNET_PACKET_MALFORMED);

    length = who_has_request(
        request,
        0x0AU,
        false,
        false,
        0U,
        0U,
        true,
        0U,
        0U,
        "GPIO22 Toggle");
    request[10] = 3U; /* Unsupported BACnet character-set identifier. */
    result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    CHECK(result.kind == BACNET_PACKET_MALFORMED);

    length = read_property_request(
        request, 8U, STATE.device_instance, 97U, false, 0U);
    result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    static const uint8_t services_supported[] = {
        0x85U, 0x06U, 0x05U, 0x44U, 0x0AU, 0x00U, 0x38U, 0x60U,
    };
    CHECK(result.kind == BACNET_PACKET_READ_PROPERTY);
    CHECK(contains_bytes(
        response,
        result.response_length,
        services_supported,
        sizeof(services_supported)));
}

static void test_device_and_binary_input_properties(void)
{
    uint8_t request[BACNET_MAX_REQUEST_BYTES];
    uint8_t response[1500];

    size_t request_length = read_property_request(
        request, 8, STATE.device_instance, 77, false, 0);
    bacnet_packet_result_t result = bacnet_handle_packet(
        request,
        request_length,
        &STATE,
        response,
        sizeof(response));
    CHECK(result.kind == BACNET_PACKET_READ_PROPERTY);
    CHECK(result.response_length > 6);
    CHECK(response[6] == 0x30 && response[7] == 1 && response[8] == 12);
    CHECK(contains_bytes(
        response,
        result.response_length,
        (const uint8_t *)STATE.device_name,
        strlen(STATE.device_name)));

    request_length = read_property_request(
        request, 8, STATE.device_instance, 76, true, 0);
    result = bacnet_handle_packet(
        request,
        request_length,
        &STATE,
        response,
        sizeof(response));
    static const uint8_t object_count_tail[] = {0x21, 0x12, 0x3F};
    CHECK(result.response_length >= sizeof(object_count_tail));
    CHECK(memcmp(
        response + result.response_length - sizeof(object_count_tail),
        object_count_tail,
        sizeof(object_count_tail)) == 0);

    request_length = read_property_request(
        request, 8, STATE.device_instance, 155, false, 0);
    result = bacnet_handle_packet(
        request,
        request_length,
        &STATE,
        response,
        sizeof(response));
    static const uint8_t database_revision_tail[] = {0x21, 0x03, 0x3F};
    CHECK(result.response_length >= sizeof(database_revision_tail));
    CHECK(memcmp(
        response + result.response_length - sizeof(database_revision_tail),
        database_revision_tail,
        sizeof(database_revision_tail)) == 0);

    request_length = read_property_request(request, 3, 20, 85, false, 0);
    result = bacnet_handle_packet(
        request,
        request_length,
        &STATE,
        response,
        sizeof(response));
    static const uint8_t inactive_tail[] = {0x91, 0x00, 0x3F};
    CHECK(result.response_length >= sizeof(inactive_tail));
    CHECK(memcmp(
        response + result.response_length - sizeof(inactive_tail),
        inactive_tail,
        sizeof(inactive_tail)) == 0);

    request_length = read_property_request(request, 3, 21, 85, false, 0);
    result = bacnet_handle_packet(
        request,
        request_length,
        &STATE,
        response,
        sizeof(response));
    static const uint8_t active_tail[] = {0x91, 0x01, 0x3F};
    CHECK(result.response_length >= sizeof(active_tail));
    CHECK(memcmp(
        response + result.response_length - sizeof(active_tail),
        active_tail,
        sizeof(active_tail)) == 0);

    request_length = read_property_request(request, 3, 22, 85, false, 0);
    result = bacnet_handle_packet(
        request,
        request_length,
        &STATE,
        response,
        sizeof(response));
    CHECK(result.response_length >= sizeof(inactive_tail));
    CHECK(memcmp(
        response + result.response_length - sizeof(inactive_tail),
        inactive_tail,
        sizeof(inactive_tail)) == 0);

    request_length = read_property_request(
        request, 8, STATE.device_instance, 76, true, 4);
    result = bacnet_handle_packet(
        request,
        request_length,
        &STATE,
        response,
        sizeof(response));
    static const uint8_t gpio22_object_tail[] = {
        0xC4, 0x00, 0xC0, 0x00, 0x16, 0x3F,
    };
    CHECK(result.response_length >= sizeof(gpio22_object_tail));
    CHECK(memcmp(
        response + result.response_length - sizeof(gpio22_object_tail),
        gpio22_object_tail,
        sizeof(gpio22_object_tail)) == 0);

    request_length = read_property_request(request, 3, 20, 111, false, 0);
    result = bacnet_handle_packet(
        request,
        request_length,
        &STATE,
        response,
        sizeof(response));
    static const uint8_t status_flags_tail[] = {0x82, 0x04, 0x40, 0x3F};
    CHECK(result.response_length >= sizeof(status_flags_tail));
    CHECK(memcmp(
        response + result.response_length - sizeof(status_flags_tail),
        status_flags_tail,
        sizeof(status_flags_tail)) == 0);

    request_length = read_property_request(request, 3, 20, 103, false, 0);
    result = bacnet_handle_packet(
        request,
        request_length,
        &STATE,
        response,
        sizeof(response));
    static const uint8_t unreliable_tail[] = {0x91, 0x07, 0x3F};
    CHECK(result.response_length >= sizeof(unreliable_tail));
    CHECK(memcmp(
        response + result.response_length - sizeof(unreliable_tail),
        unreliable_tail,
        sizeof(unreliable_tail)) == 0);

    request_length = read_property_request(request, 3, 21, 84, false, 0);
    result = bacnet_handle_packet(
        request,
        request_length,
        &STATE,
        response,
        sizeof(response));
    CHECK(result.response_length >= sizeof(active_tail));
    CHECK(memcmp(
        response + result.response_length - sizeof(active_tail),
        active_tail,
        sizeof(active_tail)) == 0);

    request_length = read_property_request(request, 2, 1000, 85, false, 0);
    result = bacnet_handle_packet(
        request,
        request_length,
        &STATE,
        response,
        sizeof(response));
    static const uint8_t temperature_tail[] = {
        0x44, 0x42, 0x2A, 0x00, 0x00, 0x3F,
    };
    CHECK(result.kind == BACNET_PACKET_READ_PROPERTY);
    CHECK(result.response_length >= sizeof(temperature_tail));
    CHECK(memcmp(
        response + result.response_length - sizeof(temperature_tail),
        temperature_tail,
        sizeof(temperature_tail)) == 0);

    request_length = read_property_request(request, 2, 1000, 117, false, 0);
    result = bacnet_handle_packet(
        request,
        request_length,
        &STATE,
        response,
        sizeof(response));
    static const uint8_t celsius_tail[] = {0x91, 0x3E, 0x3F};
    CHECK(result.response_length >= sizeof(celsius_tail));
    CHECK(memcmp(
        response + result.response_length - sizeof(celsius_tail),
        celsius_tail,
        sizeof(celsius_tail)) == 0);

    request_length = read_property_request(
        request, 8, STATE.device_instance, 76, true, 5);
    result = bacnet_handle_packet(
        request,
        request_length,
        &STATE,
        response,
        sizeof(response));
    static const uint8_t temperature_object_tail[] = {
        0xC4, 0x00, 0x80, 0x03, 0xE8, 0x3F,
    };
    CHECK(result.response_length >= sizeof(temperature_object_tail));
    CHECK(memcmp(
        response + result.response_length - sizeof(temperature_object_tail),
        temperature_object_tail,
        sizeof(temperature_object_tail)) == 0);

    request_length = read_property_request(
        request, 8, STATE.device_instance, 12, false, 0);
    result = bacnet_handle_packet(
        request,
        request_length,
        &STATE,
        response,
        sizeof(response));
    CHECK(contains_bytes(
        response,
        result.response_length,
        (const uint8_t *)STATE.application_software_version,
        strlen(STATE.application_software_version)));
}

static void test_network_port_object(void)
{
    static const uint8_t protocol_revision[] = {0x21U, 0x11U, 0x3FU};
    check_read_property_tail(
        &STATE,
        8U,
        STATE.device_instance,
        139U,
        false,
        0U,
        protocol_revision,
        sizeof(protocol_revision));

    static const uint8_t object_types[] = {
        0x85U, 0x09U, 0x07U, 0x30U, 0x80U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x80U, 0x3FU,
    };
    check_read_property_tail(
        &STATE,
        8U,
        STATE.device_instance,
        96U,
        false,
        0U,
        object_types,
        sizeof(object_types));

    static const uint8_t object_list_item[] = {
        0xC4U, 0x0EU, 0x00U, 0x00U, 0x01U, 0x3FU,
    };
    check_read_property_tail(
        &STATE,
        8U,
        STATE.device_instance,
        76U,
        true,
        15U,
        object_list_item,
        sizeof(object_list_item));

    static const uint8_t object_identifier[] = {
        0xC4U, 0x0EU, 0x00U, 0x00U, 0x01U, 0x3FU,
    };
    static const uint8_t object_type[] = {0x91U, 0x38U, 0x3FU};
    static const uint8_t normal_status[] = {0x82U, 0x04U, 0x00U, 0x3FU};
    static const uint8_t no_fault[] = {0x91U, 0x00U, 0x3FU};
    static const uint8_t zero_unsigned[] = {0x21U, 0x00U, 0x3FU};
    static const uint8_t three_unsigned[] = {0x21U, 0x03U, 0x3FU};
    static const uint8_t false_value[] = {0x10U, 0x3FU};
    static const uint8_t true_value[] = {0x11U, 0x3FU};
    static const uint8_t network_type[] = {0x91U, 0x05U, 0x3FU};
    static const uint8_t protocol_level[] = {0x91U, 0x02U, 0x3FU};
    static const uint8_t apdu_length[] = {0x22U, 0x05U, 0xC4U, 0x3FU};
    static const uint8_t link_speed[] = {
        0x44U, 0x4CU, 0xBEU, 0xBCU, 0x20U, 0x3FU,
    };
    static const uint8_t bacnet_mac[] = {
        0x65U, 0x06U, 0xC0U, 0xA8U, 0x4BU,
        0x98U, 0xBAU, 0xC0U, 0x3FU,
    };
    static const uint8_t ipv4[] = {
        0x64U, 0xC0U, 0xA8U, 0x4BU, 0x98U, 0x3FU,
    };
    static const uint8_t netmask[] = {
        0x64U, 0xFFU, 0xFFU, 0xFFU, 0x00U, 0x3FU,
    };
    static const uint8_t gateway[] = {
        0x64U, 0xC0U, 0xA8U, 0x4BU, 0x01U, 0x3FU,
    };
    static const uint8_t dns_servers[] = {
        0x64U, 0xC0U, 0xA8U, 0x4BU, 0x01U,
        0x64U, 0x08U, 0x08U, 0x08U, 0x08U,
        0x64U, 0x01U, 0x01U, 0x01U, 0x01U,
        0x3FU,
    };
    static const uint8_t dns_backup[] = {
        0x64U, 0x08U, 0x08U, 0x08U, 0x08U, 0x3FU,
    };
    static const uint8_t dns_fallback[] = {
        0x64U, 0x01U, 0x01U, 0x01U, 0x01U, 0x3FU,
    };
    static const uint8_t udp_port[] = {0x22U, 0xBAU, 0xC0U, 0x3FU};
    static const uint8_t property_count[] = {0x21U, 0x17U, 0x3FU};

    check_read_property_tail(
        &STATE, 56U, 1U, 75U, false, 0U,
        object_identifier, sizeof(object_identifier));
    check_read_property_tail(
        &STATE, 56U, 1U, 79U, false, 0U, object_type, sizeof(object_type));
    check_read_property_tail(
        &STATE, 56U, 1U, 111U, false, 0U,
        normal_status, sizeof(normal_status));
    check_read_property_tail(
        &STATE, 56U, 1U, 103U, false, 0U, no_fault, sizeof(no_fault));
    check_read_property_tail(
        &STATE, 56U, 1U, 81U, false, 0U,
        false_value, sizeof(false_value));
    check_read_property_tail(
        &STATE, 56U, 1U, 427U, false, 0U,
        network_type, sizeof(network_type));
    check_read_property_tail(
        &STATE, 56U, 1U, 482U, false, 0U,
        protocol_level, sizeof(protocol_level));
    check_read_property_tail(
        &STATE, 56U, 1U, 416U, false, 0U,
        true_value, sizeof(true_value));
    check_read_property_tail(
        &STATE, 56U, 1U, 399U, false, 0U,
        apdu_length, sizeof(apdu_length));
    check_read_property_tail(
        &STATE, 56U, 1U, 425U, false, 0U,
        zero_unsigned, sizeof(zero_unsigned));
    check_read_property_tail(
        &STATE, 56U, 1U, 426U, false, 0U, no_fault, sizeof(no_fault));
    check_read_property_tail(
        &STATE, 56U, 1U, 420U, false, 0U,
        link_speed, sizeof(link_speed));
    check_read_property_tail(
        &STATE, 56U, 1U, 423U, false, 0U,
        bacnet_mac, sizeof(bacnet_mac));
    check_read_property_tail(
        &STATE, 56U, 1U, 408U, false, 0U, no_fault, sizeof(no_fault));
    check_read_property_tail(
        &STATE, 56U, 1U, 400U, false, 0U, ipv4, sizeof(ipv4));
    check_read_property_tail(
        &STATE, 56U, 1U, 412U, false, 0U,
        udp_port, sizeof(udp_port));
    check_read_property_tail(
        &STATE, 56U, 1U, 411U, false, 0U,
        netmask, sizeof(netmask));
    check_read_property_tail(
        &STATE, 56U, 1U, 401U, false, 0U,
        gateway, sizeof(gateway));
    check_read_property_tail(
        &STATE, 56U, 1U, 406U, false, 0U,
        dns_servers, sizeof(dns_servers));
    check_read_property_tail(
        &STATE, 56U, 1U, 406U, true, 0U,
        three_unsigned, sizeof(three_unsigned));
    check_read_property_tail(
        &STATE, 56U, 1U, 406U, true, 1U,
        gateway, sizeof(gateway));
    check_read_property_tail(
        &STATE, 56U, 1U, 406U, true, 2U,
        dns_backup, sizeof(dns_backup));
    check_read_property_tail(
        &STATE, 56U, 1U, 406U, true, 3U,
        dns_fallback, sizeof(dns_fallback));
    check_read_property_tail(
        &STATE, 56U, 1U, 402U, false, 0U,
        true_value, sizeof(true_value));
    check_read_property_tail(
        &STATE, 56U, 1U, 371U, true, 0U,
        property_count, sizeof(property_count));

    uint8_t request[BACNET_MAX_REQUEST_BYTES];
    uint8_t response[1500];
    size_t length = read_property_request(
        request, 56U, 1U, 77U, false, 0U);
    bacnet_packet_result_t result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    CHECK(result.kind == BACNET_PACKET_READ_PROPERTY);
    CHECK(contains_bytes(
        response,
        result.response_length,
        (const uint8_t *)STATE.network_port_name,
        strlen(STATE.network_port_name)));

    length = read_property_request(request, 56U, 1U, 28U, false, 0U);
    result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    CHECK(result.kind == BACNET_PACKET_READ_PROPERTY);
    CHECK(contains_bytes(
        response,
        result.response_length,
        (const uint8_t *)STATE.network_port_description,
        strlen(STATE.network_port_description)));

    length = read_property_request(request, 56U, 1U, 406U, true, 4U);
    result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    static const uint8_t invalid_array_error[] = {
        0x50U, 0x01U, 0x0CU, 0x91U, 0x02U, 0x91U, 0x2AU,
    };
    CHECK(result.kind == BACNET_PACKET_READ_PROPERTY);
    CHECK(result.response_length >= sizeof(invalid_array_error));
    CHECK(memcmp(
        response + result.response_length - sizeof(invalid_array_error),
        invalid_array_error,
        sizeof(invalid_array_error)) == 0);

    bacnet_device_state_t failed = STATE;
    failed.network_port_reliability = 12U;
    failed.network_port_out_of_service = true;
    static const uint8_t failed_status[] = {0x82U, 0x04U, 0x50U, 0x3FU};
    check_read_property_tail(
        &failed, 56U, 1U, 111U, false, 0U,
        failed_status, sizeof(failed_status));

    static const test_property_reference_t selectors[] = {
        {8U, false, 0U},
        {105U, false, 0U},
        {80U, false, 0U},
    };
    length = begin_confirmed_request(request, 0x5AU, 14U);
    append_rpm_object(
        request, &length, 56U, 1U, selectors, ARRAY_LENGTH(selectors));
    length = finish_request(request, length);
    result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    CHECK(result.kind == BACNET_PACKET_READ_PROPERTY_MULTIPLE);
    CHECK(contains_bytes(
        response,
        result.response_length,
        (const uint8_t *)STATE.network_port_name,
        strlen(STATE.network_port_name)));
    CHECK(contains_bytes(
        response, result.response_length, bacnet_mac, sizeof(bacnet_mac) - 1U));

    length = who_has_request(
        request,
        0x0AU,
        false,
        false,
        0U,
        0U,
        false,
        56U,
        1U,
        NULL);
    result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    CHECK(result.kind == BACNET_PACKET_WHO_HAS);
    CHECK(result.response_length > 20U);
    CHECK(contains_bytes(
        response,
        result.response_length,
        (const uint8_t *)STATE.network_port_name,
        strlen(STATE.network_port_name)));

    length = who_has_request(
        request,
        0x0AU,
        false,
        false,
        0U,
        0U,
        true,
        0U,
        0U,
        STATE.network_port_name);
    result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    CHECK(result.kind == BACNET_PACKET_WHO_HAS);
    CHECK(result.response_length > 20U);
}

static void test_network_status_inputs(void)
{
    static const uint8_t ethernet_object[] = {
        0xC4U, 0x00U, 0xC0U, 0x03U, 0xE9U, 0x3FU,
    };
    static const uint8_t ipv4_object[] = {
        0xC4U, 0x00U, 0xC0U, 0x03U, 0xEAU, 0x3FU,
    };
    static const uint8_t active[] = {0x91U, 0x01U, 0x3FU};
    static const uint8_t normal_polarity[] = {0x91U, 0x00U, 0x3FU};

    check_read_property_tail(
        &STATE, 8U, STATE.device_instance, 76U, true, 16U,
        ethernet_object, sizeof(ethernet_object));
    check_read_property_tail(
        &STATE, 8U, STATE.device_instance, 76U, true, 17U,
        ipv4_object, sizeof(ipv4_object));
    check_read_property_tail(
        &STATE, 3U, BACNET_ETHERNET_LINK_INPUT_INSTANCE, 85U, false, 0U,
        active, sizeof(active));
    check_read_property_tail(
        &STATE, 3U, BACNET_IPV4_READY_INPUT_INSTANCE, 85U, false, 0U,
        active, sizeof(active));
    check_read_property_tail(
        &STATE, 3U, BACNET_ETHERNET_LINK_INPUT_INSTANCE, 84U, false, 0U,
        normal_polarity, sizeof(normal_polarity));

    uint8_t request[BACNET_MAX_REQUEST_BYTES];
    uint8_t response[1500];
    size_t length = read_property_request(
        request,
        3U,
        BACNET_ETHERNET_LINK_INPUT_INSTANCE,
        77U,
        false,
        0U);
    bacnet_packet_result_t result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    CHECK(result.kind == BACNET_PACKET_READ_PROPERTY);
    CHECK(contains_bytes(
        response,
        result.response_length,
        (const uint8_t *)BACNET_ETHERNET_LINK_INPUT_NAME,
        strlen(BACNET_ETHERNET_LINK_INPUT_NAME)));

    length = who_has_request(
        request,
        0x0AU,
        false,
        false,
        0U,
        0U,
        true,
        0U,
        0U,
        BACNET_IPV4_READY_INPUT_NAME);
    result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    CHECK(result.kind == BACNET_PACKET_WHO_HAS);
    CHECK(result.response_length > 20U);
    CHECK(contains_bytes(
        response,
        result.response_length,
        (const uint8_t *)BACNET_IPV4_READY_INPUT_NAME,
        strlen(BACNET_IPV4_READY_INPUT_NAME)));

    length = subscribe_cov_request(
        request,
        0x5BU,
        88U,
        3U,
        BACNET_ETHERNET_LINK_INPUT_INSTANCE,
        true,
        true,
        60U);
    result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    static const uint8_t simple_ack[] = {
        0x81U, 0x0AU, 0x00U, 0x09U, 0x01U,
        0x00U, 0x20U, 0x5BU, 0x05U,
    };
    CHECK(result.kind == BACNET_PACKET_SUBSCRIBE_COV);
    CHECK(result.cov_object_instance == BACNET_ETHERNET_LINK_INPUT_INSTANCE);
    CHECK(bytes_equal(
        response, result.response_length, simple_ack, sizeof(simple_ack)));

    bacnet_device_state_t no_ipv4 = STATE;
    no_ipv4.binary_input_values[4] = false;
    static const uint8_t inactive[] = {0x91U, 0x00U, 0x3FU};
    check_read_property_tail(
        &no_ipv4, 3U, BACNET_IPV4_READY_INPUT_INSTANCE, 85U, false, 0U,
        inactive, sizeof(inactive));
    check_read_property_tail(
        &no_ipv4, 3U, BACNET_IPV4_READY_INPUT_INSTANCE, 103U, false, 0U,
        normal_polarity, sizeof(normal_polarity));
}

static void test_boot_count_analog_value(void)
{
    static const uint8_t object_list_item[] = {
        0xC4U, 0x00U, 0x80U, 0x03U, 0xF2U, 0x3FU,
    };
    static const uint8_t present_value[] = {
        0x44U, 0x41U, 0xF8U, 0x00U, 0x00U, 0x3FU,
    };
    check_read_property_tail(
        &STATE, 8U, STATE.device_instance, 76U, true, 18U,
        object_list_item, sizeof(object_list_item));
    check_read_property_tail(
        &STATE, 2U, 1010U, 85U, false, 0U,
        present_value, sizeof(present_value));

    uint8_t request[BACNET_MAX_REQUEST_BYTES];
    uint8_t response[1500];
    const size_t length = read_property_request(
        request, 2U, 1010U, 77U, false, 0U);
    const bacnet_packet_result_t result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    CHECK(result.kind == BACNET_PACKET_READ_PROPERTY);
    CHECK(contains_bytes(
        response,
        result.response_length,
        (const uint8_t *)"Boot Count",
        strlen("Boot Count")));
}

static void test_read_property_multiple(void)
{
    uint8_t request[BACNET_MAX_REQUEST_BYTES];
    uint8_t response[1500];
    static const test_property_reference_t binary_properties[] = {
        {85, false, 0},
        {103, false, 0},
        {111, false, 0},
        {84, false, 0},
    };
    static const test_property_reference_t analog_properties[] = {
        {77, false, 0},
        {85, false, 0},
        {117, false, 0},
        {103, false, 0},
    };
    static const test_property_reference_t device_properties[] = {
        {77, false, 0},
        {12, false, 0},
    };

    size_t length = begin_confirmed_request(request, 0x44U, 14U);
    append_rpm_object(
        request,
        &length,
        3U,
        20U,
        binary_properties,
        ARRAY_LENGTH(binary_properties));
    append_rpm_object(
        request,
        &length,
        2U,
        1000U,
        analog_properties,
        ARRAY_LENGTH(analog_properties));
    append_rpm_object(
        request,
        &length,
        8U,
        STATE.device_instance,
        device_properties,
        ARRAY_LENGTH(device_properties));
    length = finish_request(request, length);

    bacnet_packet_result_t result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    CHECK(result.kind == BACNET_PACKET_READ_PROPERTY_MULTIPLE);
    CHECK(result.invoke_id == 0x44U);
    CHECK(result.response_length > 9U);
    CHECK(response[6] == 0x30U);
    CHECK(response[7] == 0x44U);
    CHECK(response[8] == 14U);

    static const uint8_t binary_object[] = {0x0C, 0x00, 0xC0, 0x00, 0x14};
    static const uint8_t inactive_value[] = {
        0x29, 0x55, 0x4E, 0x91, 0x00, 0x4F,
    };
    static const uint8_t unreliable_value[] = {
        0x29, 0x67, 0x4E, 0x91, 0x07, 0x4F,
    };
    static const uint8_t fault_status[] = {
        0x29, 0x6F, 0x4E, 0x82, 0x04, 0x40, 0x4F,
    };
    static const uint8_t normal_polarity[] = {
        0x29, 0x54, 0x4E, 0x91, 0x00, 0x4F,
    };
    static const uint8_t analog_object[] = {0x0C, 0x00, 0x80, 0x03, 0xE8};
    static const uint8_t temperature_value[] = {
        0x29, 0x55, 0x4E, 0x44, 0x42, 0x2A, 0x00, 0x00, 0x4F,
    };
    static const uint8_t celsius_units[] = {
        0x29, 0x75, 0x4E, 0x91, 0x3E, 0x4F,
    };
    CHECK(contains_bytes(
        response, result.response_length, binary_object, sizeof(binary_object)));
    CHECK(contains_bytes(
        response,
        result.response_length,
        inactive_value,
        sizeof(inactive_value)));
    CHECK(contains_bytes(
        response,
        result.response_length,
        unreliable_value,
        sizeof(unreliable_value)));
    CHECK(contains_bytes(
        response, result.response_length, fault_status, sizeof(fault_status)));
    CHECK(contains_bytes(
        response,
        result.response_length,
        normal_polarity,
        sizeof(normal_polarity)));
    CHECK(contains_bytes(
        response, result.response_length, analog_object, sizeof(analog_object)));
    CHECK(contains_bytes(
        response,
        result.response_length,
        temperature_value,
        sizeof(temperature_value)));
    CHECK(contains_bytes(
        response,
        result.response_length,
        celsius_units,
        sizeof(celsius_units)));
    CHECK(contains_bytes(
        response,
        result.response_length,
        (const uint8_t *)STATE.device_name,
        strlen(STATE.device_name)));
    CHECK(contains_bytes(
        response,
        result.response_length,
        (const uint8_t *)STATE.application_software_version,
        strlen(STATE.application_software_version)));

    static const test_property_reference_t selectors[] = {
        {8, false, 0}, /* all */
    };
    length = begin_confirmed_request(request, 0x45U, 14U);
    append_rpm_object(request, &length, 3U, 21U, selectors, 1U);
    length = finish_request(request, length);
    result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    CHECK(result.kind == BACNET_PACKET_READ_PROPERTY_MULTIPLE);
    CHECK(result.response_length > 9U);
    CHECK(contains_bytes(
        response,
        result.response_length,
        (const uint8_t *)"Active",
        strlen("Active")));
    CHECK(contains_bytes(
        response,
        result.response_length,
        (const uint8_t *)"Inactive",
        strlen("Inactive")));

    static const test_property_reference_t required_selector[] = {
        {105, false, 0},
    };
    length = begin_confirmed_request(request, 0x47U, 14U);
    append_rpm_object(request, &length, 3U, 21U, required_selector, 1U);
    length = finish_request(request, length);
    result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    CHECK(result.kind == BACNET_PACKET_READ_PROPERTY_MULTIPLE);
    CHECK(contains_bytes(
        response,
        result.response_length,
        (const uint8_t *)STATE.binary_input_names[1],
        strlen(STATE.binary_input_names[1])));
    CHECK(!contains_bytes(
        response,
        result.response_length,
        (const uint8_t *)"Inactive",
        strlen("Inactive")));

    static const test_property_reference_t optional_selector[] = {
        {80, false, 0},
    };
    length = begin_confirmed_request(request, 0x48U, 14U);
    append_rpm_object(request, &length, 3U, 21U, optional_selector, 1U);
    append_rpm_object(request, &length, 2U, 1000U, optional_selector, 1U);
    append_rpm_object(
        request,
        &length,
        8U,
        STATE.device_instance,
        optional_selector,
        1U);
    length = finish_request(request, length);
    result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    CHECK(result.kind == BACNET_PACKET_READ_PROPERTY_MULTIPLE);
    CHECK(contains_bytes(
        response,
        result.response_length,
        (const uint8_t *)"Inactive",
        strlen("Inactive")));
    CHECK(contains_bytes(
        response,
        result.response_length,
        (const uint8_t *)STATE.analog_value_descriptions[0],
        strlen(STATE.analog_value_descriptions[0])));
    CHECK(contains_bytes(
        response,
        result.response_length,
        (const uint8_t *)STATE.description,
        strlen(STATE.description)));

    static const test_property_reference_t unknown_property[] = {
        {999, false, 0},
    };
    length = begin_confirmed_request(request, 0x46U, 14U);
    append_rpm_object(
        request, &length, 3U, 20U, unknown_property, 1U);
    length = finish_request(request, length);
    result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    static const uint8_t property_error[] = {
        0x2A, 0x03, 0xE7, 0x5E, 0x91, 0x02, 0x91, 0x20, 0x5F,
    };
    CHECK(result.kind == BACNET_PACKET_READ_PROPERTY_MULTIPLE);
    CHECK(contains_bytes(
        response,
        result.response_length,
        property_error,
        sizeof(property_error)));

    length = begin_confirmed_request(request, 0x49U, 14U);
    for (size_t index = 0; index < 8U; ++index) {
        append_rpm_object(
            request,
            &length,
            8U,
            STATE.device_instance,
            selectors,
            1U);
    }
    length = finish_request(request, length);
    result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    static const uint8_t overflow_abort[] = {0x71, 0x49, 0x04};
    CHECK(result.kind == BACNET_PACKET_READ_PROPERTY_MULTIPLE);
    CHECK(result.response_length == 9U);
    CHECK(memcmp(response + 6U, overflow_abort, sizeof(overflow_abort)) == 0);
}

static void test_subscribe_cov_and_notifications(void)
{
    uint8_t request[128];
    uint8_t response[1500];
    size_t length = subscribe_cov_request(
        request, 0x51U, 77U, 3U, 21U, true, true, 120U);
    bacnet_packet_result_t result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    static const uint8_t simple_ack[] = {
        0x81, 0x0A, 0x00, 0x09, 0x01, 0x00, 0x20, 0x51, 0x05,
    };
    CHECK(result.kind == BACNET_PACKET_SUBSCRIBE_COV);
    CHECK(result.invoke_id == 0x51U);
    CHECK(result.cov_process_id == 77U);
    CHECK(result.cov_object_instance == 21U);
    CHECK(result.cov_lifetime_seconds == 120U);
    CHECK(result.cov_confirmed);
    CHECK(!result.cov_cancel);
    CHECK(bytes_equal(
        response, result.response_length, simple_ack, sizeof(simple_ack)));

    length = subscribe_cov_request(
        request, 0x52U, 77U, 3U, 21U, false, false, 0U);
    result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    CHECK(result.kind == BACNET_PACKET_SUBSCRIBE_COV);
    CHECK(result.cov_cancel);
    CHECK(!result.cov_confirmed);
    CHECK(result.cov_object_instance == 21U);
    CHECK(result.response_length == 9U);

    length = subscribe_cov_request(
        request, 0x53U, 77U, 3U, 999U, true, false, 60U);
    result = bacnet_handle_packet(
        request, length, &STATE, response, sizeof(response));
    static const uint8_t unknown_object_error[] = {
        0x50, 0x53, 0x05, 0x91, 0x01, 0x91, 0x1F,
    };
    CHECK(result.kind == BACNET_PACKET_SUBSCRIBE_COV);
    CHECK(result.response_length == 6U + sizeof(unknown_object_error));
    CHECK(memcmp(
        response + 6U,
        unknown_object_error,
        sizeof(unknown_object_error)) == 0);

    static const uint8_t expected_unconfirmed_notification[] = {
        0x81, 0x0A, 0x00, 0x25, 0x01, 0x00, 0x10, 0x02,
        0x09, 0x4D, 0x1C, 0x02, 0x09, 0x23, 0xD9,
        0x2C, 0x00, 0xC0, 0x00, 0x15, 0x39, 0x78, 0x4E,
        0x09, 0x55, 0x2E, 0x91, 0x01, 0x2F,
        0x09, 0x6F, 0x2E, 0x82, 0x04, 0x00, 0x2F, 0x4F,
    };
    length = bacnet_encode_cov_notification(
        &STATE,
        1U,
        77U,
        120U,
        false,
        0U,
        response,
        sizeof(response));
    CHECK(bytes_equal(
        response,
        length,
        expected_unconfirmed_notification,
        sizeof(expected_unconfirmed_notification)));

    length = bacnet_encode_cov_notification(
        &STATE,
        0U,
        1U,
        0U,
        true,
        0x61U,
        response,
        sizeof(response));
    CHECK(length > 9U);
    CHECK(response[6] == 0x00U);
    CHECK(response[8] == 0x61U);
    CHECK(response[9] == 0x01U);
    static const uint8_t confirmed_fault_value[] = {
        0x09, 0x6F, 0x2E, 0x82, 0x04, 0x40, 0x2F,
    };
    CHECK(contains_bytes(
        response,
        length,
        confirmed_fault_value,
        sizeof(confirmed_fault_value)));

    static const uint8_t cov_ack[] = {
        0x81, 0x0A, 0x00, 0x09, 0x01, 0x00, 0x20, 0x61, 0x01,
    };
    result = bacnet_handle_packet(
        cov_ack, sizeof(cov_ack), &STATE, response, sizeof(response));
    CHECK(result.kind == BACNET_PACKET_COV_ACK);
    CHECK(result.invoke_id == 0x61U);
    CHECK(!result.cov_ack_error);

    static const uint8_t cov_abort[] = {
        0x81, 0x0A, 0x00, 0x09, 0x01, 0x00, 0x71, 0x61, 0x04,
    };
    result = bacnet_handle_packet(
        cov_abort, sizeof(cov_abort), &STATE, response, sizeof(response));
    CHECK(result.kind == BACNET_PACKET_COV_ACK);
    CHECK(result.invoke_id == 0x61U);
    CHECK(result.cov_ack_error);
}

static void test_errors_and_malformed_input(void)
{
    static const uint8_t who_is_exact[] = {
        0x81, 0x0A, 0x00, 0x10, 0x01, 0x00, 0x10, 0x08,
        0x0B, 0x09, 0x23, 0xD9, 0x1B, 0x09, 0x23, 0xD9,
    };
    static const uint8_t missing_destination_hop[] = {
        0x81, 0x0B, 0x00, 0x09, 0x01, 0x20, 0xFF, 0xFF, 0x00,
    };
    static const uint8_t invalid_global_destination_address[] = {
        0x81, 0x0B, 0x00, 0x0D, 0x01, 0x20, 0xFF, 0xFF,
        0x01, 0xAA, 0xFF, 0x10, 0x08,
    };
    static const uint8_t invalid_source_address[] = {
        0x81, 0x0B, 0x00, 0x0B, 0x01, 0x08, 0x12, 0x34,
        0x00, 0x10, 0x08,
    };
    static const uint8_t remote_network_who_is[] = {
        0x81, 0x0B, 0x00, 0x0C, 0x01, 0x20, 0x00, 0x4B,
        0x00, 0xFF, 0x10, 0x08,
    };
    static const uint8_t network_layer_message[] = {
        0x81, 0x0B, 0x00, 0x08, 0x01, 0x80, 0x00, 0x00,
    };
    static const uint8_t reserved_npdu_bits[] = {
        0x81, 0x0B, 0x00, 0x08, 0x01, 0x50, 0x10, 0x08,
    };
    uint8_t request[BACNET_MAX_REQUEST_BYTES];
    uint8_t response[1500];

    for (size_t length = 0; length < sizeof(who_is_exact); ++length) {
        const bacnet_packet_result_t result = bacnet_handle_packet(
            who_is_exact, length, &STATE, response, sizeof(response));
        CHECK(result.kind == BACNET_PACKET_MALFORMED);
        CHECK(result.response_length == 0);
    }

    const struct {
        const uint8_t *frame;
        size_t length;
    } malformed_npdus[] = {
        {missing_destination_hop, sizeof(missing_destination_hop)},
        {invalid_global_destination_address,
         sizeof(invalid_global_destination_address)},
        {invalid_source_address, sizeof(invalid_source_address)},
        {reserved_npdu_bits, sizeof(reserved_npdu_bits)},
    };
    for (size_t index = 0; index < ARRAY_LENGTH(malformed_npdus); ++index) {
        const bacnet_packet_result_t result = bacnet_handle_packet(
            malformed_npdus[index].frame,
            malformed_npdus[index].length,
            &STATE,
            response,
            sizeof(response));
        CHECK(result.kind == BACNET_PACKET_MALFORMED);
        CHECK(result.response_length == 0);
        CHECK(!result.broadcast_response);
    }

    const uint8_t *ignored_npdus[] = {
        remote_network_who_is,
        network_layer_message,
    };
    const size_t ignored_lengths[] = {
        sizeof(remote_network_who_is),
        sizeof(network_layer_message),
    };
    for (size_t index = 0; index < ARRAY_LENGTH(ignored_npdus); ++index) {
        const bacnet_packet_result_t result = bacnet_handle_packet(
            ignored_npdus[index],
            ignored_lengths[index],
            &STATE,
            response,
            sizeof(response));
        CHECK(result.kind == BACNET_PACKET_IGNORED);
        CHECK(result.response_length == 0);
        CHECK(!result.broadcast_response);
    }

    size_t request_length = read_property_request(request, 3, 20, 85, false, 0);
    request[9] = 15; /* WriteProperty is outside the deliberately small surface. */
    bacnet_packet_result_t result = bacnet_handle_packet(
        request,
        request_length,
        &STATE,
        response,
        sizeof(response));
    static const uint8_t reject_apdu[] = {0x60, 0x01, 0x09};
    CHECK(result.kind == BACNET_PACKET_IGNORED);
    CHECK(result.response_length == 9);
    CHECK(!result.broadcast_response);
    CHECK(memcmp(response + 6, reject_apdu, sizeof(reject_apdu)) == 0);

    request[9] = 12;
    request[6] |= 0x08; /* Segmented request. */
    result = bacnet_handle_packet(
        request,
        request_length,
        &STATE,
        response,
        sizeof(response));
    static const uint8_t abort_apdu[] = {0x71, 0x01, 0x04};
    CHECK(result.response_length == 9);
    CHECK(memcmp(response + 6, abort_apdu, sizeof(abort_apdu)) == 0);

    request_length = read_property_request(request, 3, 999, 85, false, 0);
    result = bacnet_handle_packet(
        request,
        request_length,
        &STATE,
        response,
        sizeof(response));
    static const uint8_t unknown_object_tail[] = {
        0x50, 0x01, 0x0C, 0x91, 0x01, 0x91, 0x1F,
    };
    CHECK(result.response_length == 6 + sizeof(unknown_object_tail));
    CHECK(memcmp(response + 6, unknown_object_tail, sizeof(unknown_object_tail)) == 0);

    request_length = read_property_request(request, 3, 20, 999, false, 0);
    result = bacnet_handle_packet(
        request,
        request_length,
        &STATE,
        response,
        sizeof(response));
    static const uint8_t unknown_property_tail[] = {
        0x50, 0x01, 0x0C, 0x91, 0x02, 0x91, 0x20,
    };
    CHECK(result.response_length == 6 + sizeof(unknown_property_tail));
    CHECK(memcmp(
        response + 6,
        unknown_property_tail,
        sizeof(unknown_property_tail)) == 0);

    request_length = read_property_request(
        request, 8, STATE.device_instance, 76, true, 19);
    result = bacnet_handle_packet(
        request,
        request_length,
        &STATE,
        response,
        sizeof(response));
    static const uint8_t invalid_index_tail[] = {
        0x50, 0x01, 0x0C, 0x91, 0x02, 0x91, 0x2A,
    };
    CHECK(result.response_length == 6 + sizeof(invalid_index_tail));
    CHECK(memcmp(response + 6, invalid_index_tail, sizeof(invalid_index_tail)) == 0);

    request_length = begin_confirmed_request(request, 0x70U, 14U);
    request_length = finish_request(request, request_length);
    result = bacnet_handle_packet(
        request,
        request_length,
        &STATE,
        response,
        sizeof(response));
    static const uint8_t missing_parameter_reject[] = {0x60, 0x70, 0x04};
    CHECK(result.kind == BACNET_PACKET_READ_PROPERTY_MULTIPLE);
    CHECK(result.response_length == 9U);
    CHECK(memcmp(
        response + 6,
        missing_parameter_reject,
        sizeof(missing_parameter_reject)) == 0);

    static const test_property_reference_t one_property[] = {
        {85, false, 0},
    };
    request_length = begin_confirmed_request(request, 0x71U, 14U);
    for (size_t index = 0; index < 9U; ++index) {
        append_rpm_object(
            request,
            &request_length,
            3U,
            20U + (uint32_t)(index % 3U),
            one_property,
            1U);
    }
    request_length = finish_request(request, request_length);
    result = bacnet_handle_packet(
        request,
        request_length,
        &STATE,
        response,
        sizeof(response));
    static const uint8_t buffer_overflow_reject[] = {0x60, 0x71, 0x01};
    CHECK(result.kind == BACNET_PACKET_READ_PROPERTY_MULTIPLE);
    CHECK(result.response_length == 9U);
    CHECK(memcmp(
        response + 6,
        buffer_overflow_reject,
        sizeof(buffer_overflow_reject)) == 0);

    request_length = begin_confirmed_request(request, 0x72U, 14U);
    append_context_object_id(request, &request_length, 0U, 3U, 20U);
    request[request_length++] = 0x1E;
    for (size_t index = 0; index < 49U; ++index) {
        append_context_unsigned(request, &request_length, 0U, 85U);
    }
    request[request_length++] = 0x1F;
    request_length = finish_request(request, request_length);
    result = bacnet_handle_packet(
        request,
        request_length,
        &STATE,
        response,
        sizeof(response));
    static const uint8_t reference_overflow_reject[] = {0x60, 0x72, 0x01};
    CHECK(result.kind == BACNET_PACKET_READ_PROPERTY_MULTIPLE);
    CHECK(result.response_length == 9U);
    CHECK(memcmp(
        response + 6,
        reference_overflow_reject,
        sizeof(reference_overflow_reject)) == 0);

    request_length = begin_confirmed_request(request, 0x73U, 14U);
    append_context_object_id(request, &request_length, 0U, 3U, 20U);
    request[request_length++] = 0x1E;
    append_context_unsigned(request, &request_length, 0U, 85U);
    request_length = finish_request(request, request_length);
    result = bacnet_handle_packet(
        request,
        request_length,
        &STATE,
        response,
        sizeof(response));
    static const uint8_t unterminated_list_reject[] = {0x60, 0x73, 0x04};
    CHECK(result.kind == BACNET_PACKET_READ_PROPERTY_MULTIPLE);
    CHECK(result.response_length == 9U);
    CHECK(memcmp(
        response + 6,
        unterminated_list_reject,
        sizeof(unterminated_list_reject)) == 0);

    request_length = begin_confirmed_request(request, 0x74U, 5U);
    append_context_unsigned(request, &request_length, 0U, 1U);
    append_context_object_id(request, &request_length, 1U, 3U, 20U);
    append_context_unsigned(request, &request_length, 2U, 1U);
    request_length = finish_request(request, request_length);
    result = bacnet_handle_packet(
        request,
        request_length,
        &STATE,
        response,
        sizeof(response));
    static const uint8_t incomplete_subscribe_reject[] = {0x60, 0x74, 0x04};
    CHECK(result.kind == BACNET_PACKET_SUBSCRIBE_COV);
    CHECK(result.response_length == 9U);
    CHECK(memcmp(
        response + 6,
        incomplete_subscribe_reject,
        sizeof(incomplete_subscribe_reject)) == 0);

    request_length = subscribe_cov_request(
        request, 0x75U, 1U, 3U, 999U, false, false, 0U);
    result = bacnet_handle_packet(
        request,
        request_length,
        &STATE,
        response,
        sizeof(response));
    static const uint8_t cancel_unknown_object_error[] = {
        0x50, 0x75, 0x05, 0x91, 0x01, 0x91, 0x1F,
    };
    CHECK(result.kind == BACNET_PACKET_SUBSCRIBE_COV);
    CHECK(result.response_length == 6U + sizeof(cancel_unknown_object_error));
    CHECK(memcmp(
        response + 6,
        cancel_unknown_object_error,
        sizeof(cancel_unknown_object_error)) == 0);

    static const uint8_t unrelated_error[] = {
        0x81, 0x0A, 0x00, 0x0D, 0x01, 0x00,
        0x50, 0x22, 0x0C, 0x91, 0x01, 0x91, 0x1F,
    };
    result = bacnet_handle_packet(
        unrelated_error,
        sizeof(unrelated_error),
        &STATE,
        response,
        sizeof(response));
    CHECK(result.kind == BACNET_PACKET_IGNORED);
    CHECK(result.response_length == 0U);
}

static void test_capacity_guards_and_random_frames(void)
{
    uint8_t response[1500];
    memset(response, 0xA5, sizeof(response));
    for (size_t capacity = 0; capacity <= 21; ++capacity) {
        const size_t length =
            bacnet_encode_i_am(&STATE, false, response, capacity);
        if (capacity < 21) {
            CHECK(length == 0);
        } else {
            CHECK(length == 21);
        }
        for (size_t index = capacity; index < sizeof(response); ++index) {
            CHECK(response[index] == 0xA5);
        }
        memset(response, 0xA5, sizeof(response));
    }

    for (size_t capacity = 0; capacity <= 25; ++capacity) {
        const size_t length =
            bacnet_encode_i_am(&STATE, true, response, capacity);
        if (capacity < 25) {
            CHECK(length == 0);
        } else {
            CHECK(length == 25);
        }
        for (size_t index = capacity; index < sizeof(response); ++index) {
            CHECK(response[index] == 0xA5);
        }
        memset(response, 0xA5, sizeof(response));
    }

    for (size_t capacity = 0; capacity <= 37; ++capacity) {
        const size_t length = bacnet_encode_cov_notification(
            &STATE,
            1U,
            77U,
            120U,
            false,
            0U,
            response,
            capacity);
        if (capacity < 37U) {
            CHECK(length == 0U);
        } else {
            CHECK(length == 37U);
        }
        for (size_t index = capacity; index < sizeof(response); ++index) {
            CHECK(response[index] == 0xA5);
        }
        memset(response, 0xA5, sizeof(response));
    }
    CHECK(bacnet_encode_cov_notification(
        NULL, 0U, 0U, 0U, false, 0U, response, sizeof(response)) == 0U);
    CHECK(bacnet_encode_cov_notification(
        &STATE,
        BACNET_BINARY_INPUT_COUNT,
        0U,
        0U,
        false,
        0U,
        response,
        sizeof(response)) == 0U);

    uint32_t random = 0xC0FFEEU;
    uint8_t frame[BACNET_MAX_REQUEST_BYTES + 1U];
    for (size_t iteration = 0; iteration < 10000; ++iteration) {
        random = random * 1664525U + 1013904223U;
        const size_t length = random % sizeof(frame);
        for (size_t index = 0; index < length; ++index) {
            random = random * 1664525U + 1013904223U;
            frame[index] = (uint8_t)(random >> 24);
        }
        (void)bacnet_handle_packet(
            frame, length, &STATE, response, sizeof(response));
    }
    CHECK(true);
}

static void test_ota_bearer_authentication(void)
{
    static const char token[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    static const char authorization[] =
        "Bearer 0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef";
    char changed[sizeof(authorization)];
    char copied_token[OTA_TOKEN_MAX_LENGTH + 1U];
    uint8_t embedded_token[sizeof(token) + 2U];
    char short_token[OTA_TOKEN_MIN_LENGTH];
    char long_token[OTA_TOKEN_MAX_LENGTH + 2U];

    memset(short_token, 'a', sizeof(short_token));
    short_token[sizeof(short_token) - 1U] = '\0';
    memset(long_token, 'b', sizeof(long_token));
    long_token[sizeof(long_token) - 1U] = '\0';

    CHECK(ota_token_configuration_valid(token));
    CHECK(!ota_token_configuration_valid(NULL));
    CHECK(!ota_token_configuration_valid(short_token));
    CHECK(!ota_token_configuration_valid(long_token));
    CHECK(ota_authorization_valid(
        authorization, strlen(authorization), token));
    CHECK(!ota_authorization_valid(
        authorization, strlen(authorization) - 1U, token));
    CHECK(!ota_authorization_valid(
        authorization, strlen(authorization), short_token));

    memcpy(changed, authorization, sizeof(changed));
    changed[0] = 'b';
    CHECK(!ota_authorization_valid(changed, strlen(changed), token));
    memcpy(changed, authorization, sizeof(changed));
    changed[sizeof(changed) - 2U] ^= 1;
    CHECK(!ota_authorization_valid(changed, strlen(changed), token));
    memcpy(changed, authorization, sizeof(changed));
    changed[10] = '\0';
    CHECK(!ota_authorization_valid(
        changed, sizeof(changed) - 1U, token));

    memcpy(embedded_token, token, sizeof(token));
    CHECK(ota_copy_embedded_token(
        embedded_token,
        sizeof(token),
        copied_token,
        sizeof(copied_token)));
    CHECK(strcmp(copied_token, token) == 0);

    memcpy(embedded_token, token, sizeof(token) - 1U);
    embedded_token[sizeof(token) - 1U] = '\n';
    embedded_token[sizeof(token)] = '\r';
    embedded_token[sizeof(token) + 1U] = '\0';
    CHECK(ota_copy_embedded_token(
        embedded_token,
        sizeof(embedded_token),
        copied_token,
        sizeof(copied_token)));
    CHECK(strcmp(copied_token, token) == 0);

    embedded_token[10] = '\0';
    CHECK(!ota_copy_embedded_token(
        embedded_token,
        sizeof(embedded_token),
        copied_token,
        sizeof(copied_token)));
    CHECK(!ota_copy_embedded_token(
        (const uint8_t *)token,
        strlen(token),
        copied_token,
        strlen(token)));
    CHECK(!ota_copy_embedded_token(
        NULL, 0U, copied_token, sizeof(copied_token)));
}

static void test_ota_rollback_health_gate(void)
{
    ota_health_gate_t gate = {0};

    CHECK(!ota_health_gate_sample(NULL, true, 5U));
    CHECK(!ota_health_gate_sample(&gate, true, 0U));
    CHECK(gate.consecutive_healthy_samples == 0U);
    for (size_t sample = 0; sample < 4U; ++sample) {
        CHECK(!ota_health_gate_sample(&gate, true, 5U));
        CHECK(gate.consecutive_healthy_samples == sample + 1U);
    }
    CHECK(ota_health_gate_sample(&gate, true, 5U));
    CHECK(gate.consecutive_healthy_samples == 5U);
    CHECK(ota_health_gate_sample(&gate, true, 5U));
    CHECK(gate.consecutive_healthy_samples == 5U);
    CHECK(!ota_health_gate_sample(&gate, false, 5U));
    CHECK(gate.consecutive_healthy_samples == 0U);
    CHECK(!ota_health_gate_sample(&gate, true, 5U));
    ota_health_gate_reset(&gate);
    CHECK(gate.consecutive_healthy_samples == 0U);
    ota_health_gate_reset(NULL);
}

static void test_diagnostics_time_rollover(void)
{
    const uint64_t after_legacy_rollover_ms =
        (uint64_t)UINT32_MAX + 1U;

    CHECK(diagnostics_milliseconds_from_microseconds(-1) == 0U);
    CHECK(diagnostics_milliseconds_from_microseconds(999) == 0U);
    CHECK(diagnostics_milliseconds_from_microseconds(1000) == 1U);
    CHECK(diagnostics_milliseconds_from_microseconds(
              (int64_t)(after_legacy_rollover_ms * 1000U)) ==
        after_legacy_rollover_ms);
    CHECK(diagnostics_elapsed_milliseconds(
              after_legacy_rollover_ms + 5000U,
              after_legacy_rollover_ms + 1234U) == 3766U);
    CHECK(diagnostics_elapsed_milliseconds(10U, 11U) == 0U);
    CHECK(diagnostics_heartbeat_is_healthy(
        true,
        after_legacy_rollover_ms + 5000U,
        after_legacy_rollover_ms + 2500U,
        2500U));
    CHECK(!diagnostics_heartbeat_is_healthy(
        true,
        after_legacy_rollover_ms + 5001U,
        after_legacy_rollover_ms + 2500U,
        2500U));
    CHECK(!diagnostics_heartbeat_is_healthy(
        false,
        after_legacy_rollover_ms + 5000U,
        after_legacy_rollover_ms + 2500U,
        2500U));
    CHECK(!diagnostics_heartbeat_is_healthy(true, 100U, 101U, 2500U));
}

static void test_cov_retry_payload_cache(void)
{
    cov_retry_cache_t cache = {0};
    uint8_t original[] = {0x81, 0x0A, 0x00, 0x09, 0x01, 0x00, 0x20, 0x61, 0x01};
    const uint8_t expected[sizeof(original)] = {
        0x81, 0x0A, 0x00, 0x09, 0x01, 0x00, 0x20, 0x61, 0x01,
    };

    CHECK(cov_retry_cache_data(&cache) == NULL);
    CHECK(cov_retry_cache_length(&cache) == 0U);
    CHECK(cov_retry_cache_capture(&cache, original, sizeof(original)));
    memset(original, 0, sizeof(original));
    CHECK(cov_retry_cache_length(&cache) == sizeof(expected));
    CHECK(memcmp(cov_retry_cache_data(&cache), expected, sizeof(expected)) == 0);
    CHECK(!cov_retry_cache_capture(&cache, NULL, sizeof(expected)));
    CHECK(!cov_retry_cache_capture(&cache, expected, 0U));
    CHECK(!cov_retry_cache_capture(
        &cache, expected, COV_RETRY_PAYLOAD_CAPACITY + 1U));
    cov_retry_cache_clear(&cache);
    CHECK(cov_retry_cache_data(&cache) == NULL);
    CHECK(cov_retry_cache_length(&cache) == 0U);
    cov_retry_cache_clear(NULL);
}

static void test_input_line_classifier(void)
{
    CHECK(input_line_classify(true, false, true, true) ==
        INPUT_LINE_FLOATING_OPEN);
    CHECK(input_line_classify(true, false, true, false) ==
        INPUT_LINE_EXTERNALLY_LOW);
    CHECK(input_line_classify(true, true, true, true) ==
        INPUT_LINE_EXTERNALLY_HIGH);
    CHECK(input_line_classify(true, true, true, false) ==
        INPUT_LINE_UNSTABLE);
    CHECK(input_line_classify(false, false, true, true) ==
        INPUT_LINE_UNSTABLE);
    CHECK(input_line_classify(true, false, false, true) ==
        INPUT_LINE_UNSTABLE);

    CHECK(input_line_classification_valid(INPUT_LINE_FLOATING_OPEN));
    CHECK(input_line_classification_valid(INPUT_LINE_EXTERNALLY_LOW));
    CHECK(input_line_classification_valid(INPUT_LINE_EXTERNALLY_HIGH));
    CHECK(!input_line_classification_valid(INPUT_LINE_NOT_TESTED));
    CHECK(!input_line_classification_valid(INPUT_LINE_UNSTABLE));

    CHECK(strcmp(
              input_line_classification_name(INPUT_LINE_FLOATING_OPEN),
              "floating-open") == 0);
    CHECK(strcmp(
              input_line_classification_name(INPUT_LINE_EXTERNALLY_LOW),
              "externally-low") == 0);
    CHECK(strcmp(
              input_line_classification_name(INPUT_LINE_EXTERNALLY_HIGH),
              "externally-high") == 0);
    CHECK(strcmp(
              input_line_classification_name(INPUT_LINE_UNSTABLE),
              "unstable") == 0);
    CHECK(strcmp(
              input_line_classification_name(INPUT_LINE_NOT_TESTED),
              "not-tested") == 0);
    CHECK(strcmp(
              input_line_classification_name(
                  (input_line_classification_t)99),
              "not-tested") == 0);
}

static void test_input_debounce_diagnostics(void)
{
    input_debounce_state_t state;
    input_debounce_init(&state, false);
    CHECK(state.initialized);
    CHECK(!state.stable);

    input_debounce_result_t result = {0};
    for (uint64_t now = 10U; now <= 50U; now += 10U) {
        result = input_debounce_sample(
            &state, true, 10U, 50U, now);
    }
    CHECK(result.accepted_transition);
    CHECK(result.stable);
    CHECK(state.raw_edge_count == 1U);
    CHECK(state.accepted_transition_count == 1U);
    CHECK(state.last_raw_edge_uptime_ms == 10U);
    CHECK(state.last_accepted_transition_uptime_ms == 50U);
    CHECK(!state.candidate_active);

    result = input_debounce_sample(&state, false, 10U, 50U, 60U);
    CHECK(result.raw_edge);
    CHECK(!result.accepted_transition);
    result = input_debounce_sample(&state, false, 10U, 50U, 70U);
    CHECK(!result.raw_edge);
    result = input_debounce_sample(&state, true, 10U, 50U, 80U);
    CHECK(result.raw_edge);
    CHECK(result.rejected_pulse);
    CHECK(state.stable);
    CHECK(state.raw_edge_count == 3U);
    CHECK(state.rejected_pulse_count == 1U);
    CHECK(state.last_rejected_pulse_width_ms == 20U);
    CHECK(state.last_rejected_pulse_uptime_ms == 80U);

    input_debounce_init(&state, false);
    for (unsigned pulse = 0U;
         pulse < INPUT_CHATTER_REJECTION_THRESHOLD;
         ++pulse) {
        const uint64_t start = 100U + pulse * 100U;
        result = input_debounce_sample(
            &state, true, 10U, 50U, start);
        CHECK(result.raw_edge);
        result = input_debounce_sample(
            &state, false, 10U, 50U, start + 10U);
        CHECK(result.rejected_pulse);
        CHECK(result.chatter_started ==
            (pulse + 1U == INPUT_CHATTER_REJECTION_THRESHOLD));
    }
    CHECK(state.rejected_pulse_count == INPUT_CHATTER_REJECTION_THRESHOLD);
    CHECK(state.chatter_event_count == 1U);
    CHECK(input_debounce_is_chattering(&state, 1000U));
    CHECK(!input_debounce_is_chattering(
        &state, state.chattering_until_uptime_ms));

    input_debounce_init(&state, false);
    for (unsigned pulse = 0U;
         pulse < INPUT_CHATTER_REJECTION_THRESHOLD;
         ++pulse) {
        const uint64_t start = 100U + pulse * 1900U;
        (void)input_debounce_sample(
            &state, true, 10U, 50U, start);
        result = input_debounce_sample(
            &state, false, 10U, 50U, start + 10U);
        CHECK(!result.chatter_started);
    }
    CHECK(state.chatter_event_count == 0U);

    input_debounce_init(&state, false);
    for (unsigned pulse = 0U; pulse < 3U; ++pulse) {
        const uint64_t start = 100U + pulse * 100U;
        (void)input_debounce_sample(
            &state, true, 10U, 50U, start);
        (void)input_debounce_sample(
            &state, false, 10U, 50U, start + 10U);
    }
    for (uint64_t now = 500U; now <= 540U; now += 10U) {
        result = input_debounce_sample(
            &state, true, 10U, 50U, now);
    }
    CHECK(result.accepted_transition);
    (void)input_debounce_sample(&state, false, 10U, 50U, 600U);
    result = input_debounce_sample(&state, true, 10U, 50U, 610U);
    CHECK(result.rejected_pulse);
    CHECK(!result.chatter_started);
    CHECK(state.rejection_window_count == 1U);

    input_debounce_init(&state, false);
    result = input_debounce_sample(&state, true, 10U, 0U, 10U);
    CHECK(result.accepted_transition);
    CHECK(state.stable);
    state.raw_edge_count = UINT32_MAX;
    result = input_debounce_sample(&state, false, 10U, 0U, 20U);
    CHECK(result.accepted_transition);
    CHECK(state.raw_edge_count == UINT32_MAX);
    CHECK(!input_debounce_is_chattering(NULL, 0U));
    input_debounce_init(NULL, false);
    result = input_debounce_sample(NULL, false, 10U, 50U, 0U);
    CHECK(!result.accepted_transition);
}

static firmware_config_t valid_firmware_config(void)
{
    firmware_config_t config = {
        .device_instance = 599152U,
        .database_revision = 3U,
        .input_instances = {20U, 21U, 22U},
        .bacnet_port = 47808U,
        .vendor_identifier = 999U,
        .debounce_ms = 50U,
    };
    (void)snprintf(
        config.device_name, sizeof(config.device_name), "ESP32-P4 Toggle Inputs");
    (void)snprintf(
        config.vendor_name, sizeof(config.vendor_name), "Lab placeholder");
    (void)snprintf(
        config.location, sizeof(config.location), "Uncommissioned");
    for (size_t index = 0U; index < FIRMWARE_CONFIG_INPUT_COUNT; ++index) {
        (void)snprintf(
            config.input_names[index],
            sizeof(config.input_names[index]),
            "GPIO%u Toggle",
            (unsigned)config.input_instances[index]);
    }
    config_model_finalize(&config);
    return config;
}

static void test_config_model(void)
{
    char reason[128];
    firmware_config_t config = valid_firmware_config();
    CHECK(config_model_validate(&config, reason, sizeof(reason)));
    CHECK(strcmp(reason, "ok") == 0);
    CHECK(config_model_is_valid_blob(&config));
    CHECK(config.magic == FIRMWARE_CONFIG_MAGIC);
    CHECK(config.schema == FIRMWARE_CONFIG_SCHEMA);
    CHECK(config.size == sizeof(config));
    CHECK(config.crc32 == config_model_crc32(&config));

    firmware_config_t same = config;
    same.database_revision++;
    same.crc32 ^= 1U;
    CHECK(config_model_mutable_equal(&config, &same));
    same.location[0] = 'X';
    CHECK(!config_model_mutable_equal(&config, &same));
    CHECK(!config_model_mutable_equal(NULL, &same));

    firmware_config_t invalid = config;
    invalid.device_instance = 4194303U;
    CHECK(!config_model_validate(&invalid, reason, sizeof(reason)));
    invalid = config;
    invalid.bacnet_port = 0U;
    CHECK(!config_model_validate(&invalid, reason, sizeof(reason)));
    invalid = config;
    invalid.debounce_ms = 9U;
    CHECK(!config_model_validate(&invalid, reason, sizeof(reason)));
    invalid = config;
    invalid.debounce_ms = 501U;
    CHECK(!config_model_validate(&invalid, reason, sizeof(reason)));
    invalid = config;
    invalid.input_active_low_mask = 0x08U;
    CHECK(!config_model_validate(&invalid, reason, sizeof(reason)));
    invalid = config;
    invalid.input_instances[2] = invalid.input_instances[0];
    CHECK(!config_model_validate(&invalid, reason, sizeof(reason)));
    invalid = config;
    invalid.input_instances[1] = BACNET_ETHERNET_LINK_INPUT_INSTANCE;
    CHECK(!config_model_validate(&invalid, reason, sizeof(reason)));
    CHECK(strcmp(reason, "input instance is reserved for network status") == 0);
    invalid = config;
    (void)snprintf(
        invalid.input_names[1],
        sizeof(invalid.input_names[1]),
        "%s",
        BACNET_IPV4_READY_INPUT_NAME);
    CHECK(!config_model_validate(&invalid, reason, sizeof(reason)));
    invalid = config;
    (void)snprintf(
        invalid.input_names[2],
        sizeof(invalid.input_names[2]),
        "%s",
        invalid.input_names[0]);
    CHECK(!config_model_validate(&invalid, reason, sizeof(reason)));
    invalid = config;
    memset(invalid.device_name, 'A', sizeof(invalid.device_name));
    CHECK(!config_model_validate(&invalid, reason, sizeof(reason)));
    invalid = config;
    invalid.vendor_name[0] = '\x01';
    CHECK(!config_model_validate(&invalid, reason, sizeof(reason)));
    invalid = config;
    invalid.location[0] = '\x7f';
    CHECK(!config_model_validate(&invalid, reason, sizeof(reason)));
    invalid = config;
    invalid.crc32 ^= 1U;
    CHECK(!config_model_is_valid_blob(&invalid));
    CHECK(!config_model_validate(NULL, reason, sizeof(reason)));
    config_model_finalize(NULL);
    CHECK(config_model_crc32(NULL) == 0U);
}

static network_config_t valid_network_config(void)
{
    network_config_t config = {
        .revision = 7U,
        .mode = NETWORK_ADDRESS_STATIC,
        .ipv4 = {192U, 168U, 75U, 152U},
        .netmask = {255U, 255U, 255U, 0U},
        .gateway = {192U, 168U, 75U, 1U},
        .dns = {192U, 168U, 75U, 1U},
    };
    (void)snprintf(
        config.hostname, sizeof(config.hostname), "esp32-p4-bacnet");
    network_config_finalize(&config);
    return config;
}

static void test_network_config_model(void)
{
    char reason[128];
    network_config_t config = valid_network_config();
    CHECK(network_config_validate(&config, reason, sizeof(reason)));
    CHECK(strcmp(reason, "ok") == 0);
    CHECK(network_config_is_valid_blob(&config));
    CHECK(config.magic == NETWORK_CONFIG_MAGIC);
    CHECK(config.schema == NETWORK_CONFIG_SCHEMA);
    CHECK(config.size == sizeof(config));
    CHECK(config.crc32 == network_config_crc32(&config));

    network_config_t same = config;
    same.revision++;
    same.crc32 ^= 1U;
    CHECK(network_config_mutable_equal(&config, &same));
    same.ipv4[3]++;
    CHECK(!network_config_mutable_equal(&config, &same));
    CHECK(!network_config_mutable_equal(NULL, &same));

    network_config_t dhcp = {0};
    dhcp.mode = NETWORK_ADDRESS_DHCP;
    (void)snprintf(dhcp.hostname, sizeof(dhcp.hostname), "p4-controller");
    network_config_finalize(&dhcp);
    CHECK(network_config_validate(&dhcp, reason, sizeof(reason)));
    CHECK(network_config_is_valid_blob(&dhcp));
    dhcp.ipv4[3] = 1U;
    CHECK(!network_config_validate(&dhcp, reason, sizeof(reason)));

    network_config_t invalid = config;
    invalid.mode = 2U;
    CHECK(!network_config_validate(&invalid, reason, sizeof(reason)));
    invalid = config;
    invalid.hostname[0] = '-';
    CHECK(!network_config_validate(&invalid, reason, sizeof(reason)));
    invalid = config;
    (void)snprintf(
        invalid.hostname, sizeof(invalid.hostname), "bad.hostname");
    CHECK(!network_config_validate(&invalid, reason, sizeof(reason)));
    invalid = config;
    memset(invalid.hostname, 'a', sizeof(invalid.hostname));
    CHECK(!network_config_validate(&invalid, reason, sizeof(reason)));
    invalid = config;
    invalid.netmask[3] = 1U;
    CHECK(!network_config_validate(&invalid, reason, sizeof(reason)));
    invalid = config;
    invalid.gateway[2] = 76U;
    CHECK(!network_config_validate(&invalid, reason, sizeof(reason)));
    invalid = config;
    invalid.ipv4[3] = 0U;
    CHECK(!network_config_validate(&invalid, reason, sizeof(reason)));
    invalid = config;
    invalid.ipv4[0] = 224U;
    CHECK(!network_config_validate(&invalid, reason, sizeof(reason)));
    invalid = config;
    invalid.dns[0] = 127U;
    CHECK(!network_config_validate(&invalid, reason, sizeof(reason)));
    invalid = config;
    invalid.crc32 ^= 1U;
    CHECK(!network_config_is_valid_blob(&invalid));
    CHECK(!network_config_validate(NULL, reason, sizeof(reason)));
    network_config_finalize(NULL);
    CHECK(network_config_crc32(NULL) == 0U);
}

static void test_hardware_profile(void)
{
    CHECK(hardware_profile_p1_position(20) == 35);
    CHECK(hardware_profile_p1_position(21) == 34);
    CHECK(hardware_profile_p1_position(22) == 32);
    CHECK(hardware_profile_p1_position(23) == -1);
    CHECK(strcmp(hardware_profile_binary_state(false, false), "inactive") == 0);
    CHECK(strcmp(hardware_profile_binary_state(true, false), "active") == 0);
    CHECK(strcmp(hardware_profile_binary_state(false, true), "active") == 0);
    CHECK(strcmp(hardware_profile_binary_state(true, true), "inactive") == 0);
}

static void test_diagnostics_metrics(void)
{
    diagnostics_temperature_metrics_t temperature = {0};
    diagnostics_temperature_metrics_record(
        &temperature, true, 35.5F, 0);
    CHECK(temperature.has_sample);
    CHECK(temperature.minimum_c == 35.5F);
    CHECK(temperature.maximum_c == 35.5F);
    CHECK(temperature.sample_count == 1U);
    CHECK(temperature.error_count == 0U);
    CHECK(temperature.last_result == 0);

    diagnostics_temperature_metrics_record(
        &temperature, true, 31.25F, 0);
    diagnostics_temperature_metrics_record(
        &temperature, true, 42.75F, 0);
    diagnostics_temperature_metrics_record(
        &temperature, false, 0.0F, -7);
    CHECK(temperature.minimum_c == 31.25F);
    CHECK(temperature.maximum_c == 42.75F);
    CHECK(temperature.sample_count == 3U);
    CHECK(temperature.error_count == 1U);
    CHECK(temperature.last_result == -7);

    temperature.sample_count = UINT32_MAX;
    temperature.error_count = UINT32_MAX;
    diagnostics_temperature_metrics_record(
        &temperature, true, 36.0F, 0);
    diagnostics_temperature_metrics_record(
        &temperature, false, 0.0F, -1);
    CHECK(temperature.sample_count == UINT32_MAX);
    CHECK(temperature.error_count == UINT32_MAX);
    diagnostics_temperature_metrics_record(NULL, true, 1.0F, 0);

    diagnostics_fault_log_metrics_t fault_log =
        diagnostics_fault_log_metrics(1U, 0U);
    CHECK(fault_log.total_event_count == 0U);
    CHECK(fault_log.overwritten_event_count == 0U);
    fault_log = diagnostics_fault_log_metrics(97U, 16U);
    CHECK(fault_log.total_event_count == 96U);
    CHECK(fault_log.overwritten_event_count == 80U);
    fault_log = diagnostics_fault_log_metrics(4U, 16U);
    CHECK(fault_log.total_event_count == 3U);
    CHECK(fault_log.overwritten_event_count == 0U);
    fault_log = diagnostics_fault_log_metrics(0U, 16U);
    CHECK(fault_log.total_event_count == UINT32_MAX);
    CHECK(fault_log.overwritten_event_count == UINT32_MAX - 16U);
}

static void test_subscribe_cov_capacity_error(void)
{
    uint8_t response[32];
    const size_t length = bacnet_encode_subscribe_cov_no_space(
        0x5AU, response, sizeof(response));
    static const uint8_t expected[] = {
        0x81U, 0x0AU, 0x00U, 0x0DU, 0x01U, 0x00U,
        0x50U, 0x5AU, 0x05U, 0x91U, 0x03U, 0x91U, 0x13U,
    };
    CHECK(bytes_equal(response, length, expected, sizeof(expected)));
    CHECK(bacnet_encode_subscribe_cov_no_space(
        0x5AU, response, sizeof(expected) - 1U) == 0U);
}

int main(void)
{
    test_reference_vectors();
    test_who_has_i_have();
    test_device_and_binary_input_properties();
    test_network_port_object();
    test_network_status_inputs();
    test_boot_count_analog_value();
    test_read_property_multiple();
    test_subscribe_cov_and_notifications();
    test_subscribe_cov_capacity_error();
    test_errors_and_malformed_input();
    test_capacity_guards_and_random_frames();
    test_ota_bearer_authentication();
    test_ota_rollback_health_gate();
    test_diagnostics_time_rollover();
    test_cov_retry_payload_cache();
    test_input_debounce_diagnostics();
    test_input_line_classifier();
    test_config_model();
    test_network_config_model();
    test_hardware_profile();
    test_diagnostics_metrics();
    printf("bacnet_codec_tests: %u checks passed\n", tests_run);
    return EXIT_SUCCESS;
}
