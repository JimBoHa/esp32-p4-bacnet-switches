#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bacnet_codec.h"
#include "ota_auth.h"

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
    .firmware_revision = "1.3.0",
    .application_software_version = "1.3.0 (0123456789ab)",
    .description = "Three toggle inputs",
    .database_revision = 3,
    .binary_input_instances = {20, 21, 22},
    .binary_input_names = {"GPIO20 Toggle", "GPIO21 Toggle", "GPIO22 Toggle"},
    .binary_input_descriptions = {
        "GPIO20 input",
        "GPIO21 input",
        "GPIO22 input",
    },
    .binary_input_values = {false, true, false},
    .binary_input_reliability = {7, 0, 0},
    .binary_input_active_low = {false, true, false},
    .analog_value_instances = {
        1000, 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009,
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
    },
    .analog_value_values = {
        42.5F, 123.0F, 500000.0F, 450000.0F, 1.0F,
        2.0F, 300.0F, 4.0F, 1.0F, 2.0F,
    },
    .analog_value_units = {62, 73, 95, 95, 95, 95, 95, 95, 95, 95},
    .analog_value_reliability = {0, 0, 0, 0, 0, 0, 0, 7, 0, 0},
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
    CHECK(result.broadcast_response);
    CHECK(bytes_equal(
        response,
        result.response_length,
        expected_broadcast_i_am,
        ARRAY_LENGTH(expected_broadcast_i_am)));

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
    static const uint8_t object_count_tail[] = {0x21, 0x0E, 0x3F};
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
        request, 8, STATE.device_instance, 76, true, 15);
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
}

int main(void)
{
    test_reference_vectors();
    test_device_and_binary_input_properties();
    test_read_property_multiple();
    test_subscribe_cov_and_notifications();
    test_errors_and_malformed_input();
    test_capacity_guards_and_random_frames();
    test_ota_bearer_authentication();
    printf("bacnet_codec_tests: %u checks passed\n", tests_run);
    return EXIT_SUCCESS;
}
