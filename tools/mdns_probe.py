#!/usr/bin/env python3
"""Probe and validate the controller's mDNS/DNS-SD advertisements."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import ipaddress
import socket
import struct
import time
from typing import Any, Iterable


MDNS_ADDRESS = "224.0.0.251"
MDNS_PORT = 5353
DNS_CLASS_IN = 1
DNS_CLASS_UNICAST_RESPONSE = 0x8000
DNS_TYPE_A = 1
DNS_TYPE_PTR = 12
DNS_TYPE_TXT = 16
DNS_TYPE_SRV = 33
MAX_DNS_MESSAGE_BYTES = 9000


class MdnsProbeError(RuntimeError):
    """An invalid packet or failed discovery check."""


@dataclass(frozen=True)
class DnsRecord:
    name: str
    record_type: int
    ttl: int
    value: Any


def normalized_name(name: str) -> str:
    return name.rstrip(".").casefold() + "."


def encode_name(name: str) -> bytes:
    output = bytearray()
    labels = name.rstrip(".").split(".")
    if not labels or any(not label for label in labels):
        raise MdnsProbeError(f"invalid DNS name: {name!r}")
    for label in labels:
        encoded = label.encode("utf-8")
        if len(encoded) > 63:
            raise MdnsProbeError(f"DNS label is longer than 63 bytes: {label!r}")
        output.append(len(encoded))
        output.extend(encoded)
    output.append(0)
    return bytes(output)


def decode_name(message: bytes, offset: int) -> tuple[str, int]:
    labels: list[str] = []
    return_offset: int | None = None
    visited: set[int] = set()
    while True:
        if offset >= len(message):
            raise MdnsProbeError("DNS name extends beyond packet")
        if offset in visited:
            raise MdnsProbeError("DNS compression pointer loop")
        visited.add(offset)
        length = message[offset]
        if length & 0xC0 == 0xC0:
            if offset + 1 >= len(message):
                raise MdnsProbeError("truncated DNS compression pointer")
            pointer = ((length & 0x3F) << 8) | message[offset + 1]
            if pointer >= len(message):
                raise MdnsProbeError("DNS compression pointer is out of range")
            if return_offset is None:
                return_offset = offset + 2
            offset = pointer
            continue
        if length & 0xC0:
            raise MdnsProbeError("unsupported DNS label encoding")
        offset += 1
        if length == 0:
            return ".".join(labels) + ".", (
                offset if return_offset is None else return_offset
            )
        if offset + length > len(message):
            raise MdnsProbeError("truncated DNS label")
        try:
            label = message[offset : offset + length].decode("utf-8")
        except UnicodeDecodeError as error:
            raise MdnsProbeError("DNS label is not valid UTF-8") from error
        labels.append(label)
        offset += length
        if len(labels) > 128:
            raise MdnsProbeError("DNS name has too many labels")


def build_query(questions: Iterable[tuple[str, int]]) -> bytes:
    encoded_questions = list(questions)
    if not encoded_questions:
        raise MdnsProbeError("at least one DNS question is required")
    message = bytearray(struct.pack("!HHHHHH", 0, 0, len(encoded_questions), 0, 0, 0))
    for name, record_type in encoded_questions:
        message.extend(encode_name(name))
        message.extend(
            struct.pack(
                "!HH", record_type, DNS_CLASS_IN | DNS_CLASS_UNICAST_RESPONSE
            )
        )
    return bytes(message)


def _parse_txt(data: bytes) -> dict[str, str]:
    values: dict[str, str] = {}
    offset = 0
    while offset < len(data):
        length = data[offset]
        offset += 1
        if offset + length > len(data):
            raise MdnsProbeError("truncated DNS TXT item")
        try:
            item = data[offset : offset + length].decode("utf-8")
        except UnicodeDecodeError as error:
            raise MdnsProbeError("DNS TXT item is not valid UTF-8") from error
        offset += length
        key, separator, value = item.partition("=")
        if not key:
            raise MdnsProbeError("DNS TXT item has an empty key")
        values[key] = value if separator else ""
    return values


def parse_message(message: bytes) -> list[DnsRecord]:
    if len(message) < 12:
        raise MdnsProbeError("truncated DNS header")
    _, flags, question_count, answer_count, authority_count, additional_count = (
        struct.unpack_from("!HHHHHH", message)
    )
    if flags & 0x8000 == 0:
        return []
    offset = 12
    for _ in range(question_count):
        _, offset = decode_name(message, offset)
        if offset + 4 > len(message):
            raise MdnsProbeError("truncated DNS question")
        offset += 4

    records: list[DnsRecord] = []
    for _ in range(answer_count + authority_count + additional_count):
        name, offset = decode_name(message, offset)
        if offset + 10 > len(message):
            raise MdnsProbeError("truncated DNS record header")
        record_type, _, ttl, data_length = struct.unpack_from("!HHIH", message, offset)
        offset += 10
        data_offset = offset
        data_end = data_offset + data_length
        if data_end > len(message):
            raise MdnsProbeError("truncated DNS record data")

        value: Any = message[data_offset:data_end]
        if record_type == DNS_TYPE_A:
            if data_length != 4:
                raise MdnsProbeError("invalid DNS A record length")
            value = socket.inet_ntoa(value)
        elif record_type == DNS_TYPE_PTR:
            value, value_end = decode_name(message, data_offset)
            if value_end != data_end:
                raise MdnsProbeError("invalid DNS PTR record length")
        elif record_type == DNS_TYPE_SRV:
            if data_length < 7:
                raise MdnsProbeError("invalid DNS SRV record length")
            priority, weight, port = struct.unpack_from("!HHH", message, data_offset)
            target, target_end = decode_name(message, data_offset + 6)
            if target_end != data_end:
                raise MdnsProbeError("invalid DNS SRV record length")
            value = {
                "priority": priority,
                "weight": weight,
                "port": port,
                "target": target,
            }
        elif record_type == DNS_TYPE_TXT:
            value = _parse_txt(value)
        records.append(DnsRecord(name, record_type, ttl, value))
        offset = data_end
    return records


def _matching_records(
    records: Iterable[DnsRecord], name: str, record_type: int
) -> list[DnsRecord]:
    expected = normalized_name(name)
    return [
        record
        for record in records
        if record.record_type == record_type
        and normalized_name(record.name) == expected
        and record.ttl > 0
    ]


def _service_failures(
    records: list[DnsRecord],
    service: str,
    protocol: str,
    hostname: str,
    instance_name: str | None,
    expected_port: int,
    expected_txt: dict[str, str],
) -> list[str]:
    service_name = f"{service}.{protocol}.local."
    ptr_records = _matching_records(records, service_name, DNS_TYPE_PTR)
    if not ptr_records:
        return [f"no PTR response for {service_name}"]
    targets = [str(record.value) for record in ptr_records]
    if instance_name is not None:
        expected_instance = normalized_name(
            f"{instance_name}.{service}.{protocol}.local."
        )
        targets = [
            target
            for target in targets
            if normalized_name(target) == expected_instance
        ]
        if not targets:
            return [f"no PTR target for instance {instance_name!r}"]

    expected_host = normalized_name(f"{hostname}.local.")
    failures: list[str] = []
    for target in targets:
        srv_records = _matching_records(records, target, DNS_TYPE_SRV)
        valid_srv = any(
            isinstance(record.value, dict)
            and record.value.get("port") == expected_port
            and normalized_name(str(record.value.get("target", ""))) == expected_host
            for record in srv_records
        )
        txt_records = _matching_records(records, target, DNS_TYPE_TXT)
        valid_txt = any(
            isinstance(record.value, dict)
            and all(record.value.get(key) == value for key, value in expected_txt.items())
            for record in txt_records
        )
        if valid_srv and valid_txt:
            return []
        if not valid_srv:
            failures.append(
                f"{target} has no SRV record for {hostname}.local:{expected_port}"
            )
        if not valid_txt:
            failures.append(f"{target} is missing expected TXT metadata")
    return failures


def validate_discovery(
    records: list[DnsRecord],
    hostname: str,
    expected_address: str,
    instance_name: str | None,
    https_port: int | None,
    bacnet_port: int,
    device_instance: int,
    vendor_identifier: int,
    firmware_version: str | None,
) -> list[str]:
    failures: list[str] = []
    address_records = _matching_records(
        records, f"{hostname}.local.", DNS_TYPE_A
    )
    if not any(record.value == expected_address for record in address_records):
        failures.append(
            f"{hostname}.local has no A record for {expected_address}"
        )
    common_txt = {"project": "esp32_p4_bacnet_switches"}
    if firmware_version is not None:
        common_txt["version"] = firmware_version
    if https_port is not None:
        failures.extend(
            _service_failures(
                records,
                "_https",
                "_tcp",
                hostname,
                instance_name,
                https_port,
                {**common_txt, "path": "/ota/status", "auth": "bearer", "read_auth": "none"},
            )
        )
    failures.extend(
        _service_failures(
            records,
            "_bacnet",
            "_udp",
            hostname,
            instance_name,
            bacnet_port,
            {
                **common_txt,
                "device": str(device_instance),
                "vendor": str(vendor_identifier),
            },
        )
    )
    return failures


def probe(
    interface_address: str,
    hostname: str,
    expected_address: str,
    instance_name: str | None,
    https_port: int | None,
    bacnet_port: int,
    device_instance: int,
    vendor_identifier: int,
    firmware_version: str | None,
    timeout: float,
) -> list[DnsRecord]:
    try:
        interface = str(ipaddress.IPv4Address(interface_address))
        target = str(ipaddress.IPv4Address(expected_address))
    except ipaddress.AddressValueError as error:
        raise MdnsProbeError(f"invalid IPv4 address: {error}") from error
    if timeout <= 0:
        raise MdnsProbeError("timeout must be greater than zero")
    questions = [
        (f"{hostname}.local.", DNS_TYPE_A),
        ("_bacnet._udp.local.", DNS_TYPE_PTR),
    ]
    if https_port is not None:
        questions.append(("_https._tcp.local.", DNS_TYPE_PTR))
    query = build_query(questions)
    records: list[DnsRecord] = []
    seen: set[tuple[str, int, str]] = set()
    malformed: list[str] = []
    deadline = time.monotonic() + timeout
    next_send = 0.0

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP) as sock:
        sock.bind((interface, 0))
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(interface))
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
        sock.settimeout(min(0.25, timeout))
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now >= next_send:
                sock.sendto(query, (MDNS_ADDRESS, MDNS_PORT))
                next_send = now + min(1.0, max(0.25, timeout / 3.0))
            try:
                message, _ = sock.recvfrom(MAX_DNS_MESSAGE_BYTES)
            except socket.timeout:
                continue
            try:
                parsed = parse_message(message)
            except MdnsProbeError as error:
                malformed.append(str(error))
                continue
            for record in parsed:
                identity = (
                    normalized_name(record.name),
                    record.record_type,
                    repr(record.value),
                )
                if identity not in seen:
                    records.append(record)
                    seen.add(identity)
            failures = validate_discovery(
                records,
                hostname,
                target,
                instance_name,
                https_port,
                bacnet_port,
                device_instance,
                vendor_identifier,
                firmware_version,
            )
            if not failures:
                return records

    failures = validate_discovery(
        records,
        hostname,
        target,
        instance_name,
        https_port,
        bacnet_port,
        device_instance,
        vendor_identifier,
        firmware_version,
    )
    detail = "; ".join(failures)
    if malformed:
        detail += f"; ignored {len(malformed)} malformed response(s)"
    raise MdnsProbeError(detail or "mDNS discovery timed out")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interface", required=True, help="local IPv4 interface address")
    parser.add_argument("--hostname", required=True, help="expected .local hostname label")
    parser.add_argument("--address", required=True, help="expected controller IPv4 address")
    parser.add_argument("--instance", help="expected DNS-SD instance name")
    parser.add_argument("--https-port", type=int, default=443)
    parser.add_argument("--no-https", action="store_true")
    parser.add_argument("--bacnet-port", type=int, default=47808)
    parser.add_argument("--device-instance", required=True, type=int)
    parser.add_argument("--vendor-identifier", type=int, default=999)
    parser.add_argument("--firmware-version")
    parser.add_argument("--timeout", type=float, default=4.0)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        records = probe(
            args.interface,
            args.hostname,
            args.address,
            args.instance,
            None if args.no_https else args.https_port,
            args.bacnet_port,
            args.device_instance,
            args.vendor_identifier,
            args.firmware_version,
            args.timeout,
        )
    except (MdnsProbeError, OSError) as error:
        print(f"FAIL: {error}")
        return 1
    print(
        f"PASS: {args.hostname}.local -> {args.address}; "
        f"BACnet/IP UDP {args.bacnet_port}"
        + ("" if args.no_https else f"; HTTPS TCP {args.https_port}")
        + f"; {len(records)} unique DNS records"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
