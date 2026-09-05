#!/usr/bin/env python3
"""Host tests for the mDNS/DNS-SD probe."""

from __future__ import annotations

import socket
import struct
import unittest

from tools import mdns_probe


def record(name: str, record_type: int, value: bytes, ttl: int = 120) -> bytes:
    return (
        mdns_probe.encode_name(name)
        + struct.pack("!HHIH", record_type, 0x8001, ttl, len(value))
        + value
    )


def name_value(name: str) -> bytes:
    return mdns_probe.encode_name(name)


def txt_value(values: dict[str, str]) -> bytes:
    output = bytearray()
    for key, value in values.items():
        item = f"{key}={value}".encode()
        output.append(len(item))
        output.extend(item)
    return bytes(output)


class MdnsProbeTests(unittest.TestCase):
    def test_query_requests_unicast_responses(self) -> None:
        query = mdns_probe.build_query([("device.local.", mdns_probe.DNS_TYPE_A)])
        self.assertEqual(struct.unpack_from("!H", query, 4)[0], 1)
        self.assertTrue(query.endswith(struct.pack("!HH", 1, 0x8001)))

    def test_parse_and_validate_complete_advertisement(self) -> None:
        instance = "P4 BACnet Controller"
        hostname = "esp32-p4-bacnet"
        host_fqdn = f"{hostname}.local."
        https_target = f"{instance}._https._tcp.local."
        bacnet_target = f"{instance}._bacnet._udp.local."
        records = [
            record(host_fqdn, mdns_probe.DNS_TYPE_A, socket.inet_aton("192.168.75.152")),
            record("_https._tcp.local.", mdns_probe.DNS_TYPE_PTR, name_value(https_target)),
            record(
                https_target,
                mdns_probe.DNS_TYPE_SRV,
                struct.pack("!HHH", 0, 0, 443) + name_value(host_fqdn),
            ),
            record(
                https_target,
                mdns_probe.DNS_TYPE_TXT,
                txt_value(
                    {
                        "path": "/ota/status",
                        "auth": "bearer",
                        "project": "esp32_p4_bacnet_switches",
                        "version": "1.14.0",
                    }
                ),
            ),
            record("_bacnet._udp.local.", mdns_probe.DNS_TYPE_PTR, name_value(bacnet_target)),
            record(
                bacnet_target,
                mdns_probe.DNS_TYPE_SRV,
                struct.pack("!HHH", 0, 0, 47808) + name_value(host_fqdn),
            ),
            record(
                bacnet_target,
                mdns_probe.DNS_TYPE_TXT,
                txt_value(
                    {
                        "device": "599152",
                        "vendor": "999",
                        "project": "esp32_p4_bacnet_switches",
                        "version": "1.14.0",
                    }
                ),
            ),
        ]
        packet = struct.pack("!HHHHHH", 0, 0x8400, 0, len(records), 0, 0) + b"".join(records)
        parsed = mdns_probe.parse_message(packet)
        self.assertEqual(len(parsed), len(records))
        self.assertEqual(
            mdns_probe.validate_discovery(
                parsed,
                hostname,
                "192.168.75.152",
                instance,
                443,
                47808,
                599152,
                999,
                "1.14.0",
            ),
            [],
        )
        self.assertIn("auth", parsed[3].value)

    def test_validation_rejects_wrong_bacnet_port(self) -> None:
        target = "P4._bacnet._udp.local."
        packet_records = [
            record(
                "esp32-p4.local.",
                mdns_probe.DNS_TYPE_A,
                socket.inet_aton("192.0.2.10"),
            ),
            record("_bacnet._udp.local.", mdns_probe.DNS_TYPE_PTR, name_value(target)),
            record(
                target,
                mdns_probe.DNS_TYPE_SRV,
                struct.pack("!HHH", 0, 0, 1234) + name_value("esp32-p4.local."),
            ),
            record(
                target,
                mdns_probe.DNS_TYPE_TXT,
                txt_value(
                    {
                        "device": "1",
                        "vendor": "999",
                        "project": "esp32_p4_bacnet_switches",
                    }
                ),
            ),
        ]
        failures = mdns_probe.validate_discovery(
            [
                item
                for packet in packet_records
                for item in mdns_probe.parse_message(
                    struct.pack("!HHHHHH", 0, 0x8400, 0, 1, 0, 0) + packet
                )
            ],
            "esp32-p4",
            "192.0.2.10",
            "P4",
            None,
            47808,
            1,
            999,
            None,
        )
        self.assertTrue(any("47808" in failure for failure in failures))

    def test_parser_rejects_compression_pointer_cycle(self) -> None:
        packet = struct.pack("!HHHHHH", 0, 0x8400, 0, 1, 0, 0)
        packet += b"\xc0\x0c" + struct.pack("!HHIH", 1, 1, 120, 4)
        packet += socket.inet_aton("192.0.2.1")
        with self.assertRaisesRegex(mdns_probe.MdnsProbeError, "pointer loop"):
            mdns_probe.parse_message(packet)

    def test_parser_decodes_compressed_ptr_target(self) -> None:
        owner = mdns_probe.encode_name("_https._tcp.local.")
        target = b"\x02P4\xc0\x0c"
        packet = struct.pack("!HHHHHH", 0, 0x8400, 0, 1, 0, 0)
        packet += owner + struct.pack("!HHIH", 12, 1, 120, len(target)) + target
        parsed = mdns_probe.parse_message(packet)
        self.assertEqual(parsed[0].value, "P4._https._tcp.local.")


if __name__ == "__main__":
    unittest.main()
