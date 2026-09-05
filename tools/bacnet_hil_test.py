#!/usr/bin/env python3
"""Run repeatable, non-actuating BACnet/IP acceptance tests on the ESP32-P4."""

from __future__ import annotations

import argparse
import asyncio
from contextlib import AsyncExitStack
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
import hashlib
import importlib.metadata
import ipaddress
import json
import os
from pathlib import Path
import re
import sys
import time
from typing import Any, Awaitable


REQUIRED_BACPYPES3_VERSION = "0.0.106"
MAX_DEVICE_INSTANCE = 4_194_302
PHYSICAL_INPUT_INSTANCES = (20, 21, 22)
STATUS_INPUT_INSTANCES = (1001, 1002)
COMPAT_ANALOG_VALUE_INSTANCES = tuple(range(1000, 1010))
APPENDED_ANALOG_VALUE_INSTANCES = (1010,)
ANALOG_VALUE_INSTANCES = (
    *COMPAT_ANALOG_VALUE_INSTANCES,
    *APPENDED_ANALOG_VALUE_INSTANCES,
)
COV_CAPACITY = 8
FIRMWARE_DATABASE_REVISION_OFFSET = 3
MAX_HTTP_RESPONSE_BYTES = 1024 * 1024
EXPECTED_HARDWARE_PROFILE = {
    "board_model": "Waveshare ESP32-P4-POE-ETH",
    "header": "P1",
    "supply": {"label": "3V3", "position": 36, "nominal_volts": 3.3},
    "input_circuit": {
        "contact_type": "dry-contact",
        "internal_pull": "down",
        "input_only": True,
        "closed_to_supply": True,
        "closed_raw_level": True,
        "open_raw_level": False,
        "recommended_external_pull_down_ohms": 10000,
    },
    "inputs": [
        {
            "channel": channel,
            "gpio": gpio,
            "bacnet_binary_input_instance": gpio,
            "header_position": position,
            "configured_active_low": False,
            "open_state": "inactive",
            "closed_state": "active",
        }
        for channel, gpio, position in ((1, 20, 35), (2, 21, 34), (3, 22, 32))
    ],
}


class HilError(RuntimeError):
    """An expected hardware-acceptance failure."""


@dataclass
class CheckResult:
    name: str
    outcome: str
    detail: str
    elapsed_seconds: float = 0.0


@dataclass
class TestReport:
    started_at: str
    target: dict[str, Any]
    options: dict[str, Any]
    bacpypes3_version: str = "unavailable"
    finished_at: str | None = None
    checks: list[CheckResult] = field(default_factory=list)

    def add(
        self,
        name: str,
        outcome: str,
        detail: str,
        elapsed_seconds: float = 0.0,
    ) -> None:
        self.checks.append(CheckResult(name, outcome, detail, elapsed_seconds))
        marker = {"pass": "PASS", "fail": "FAIL", "skip": "SKIP"}[outcome]
        print(f"[{marker}] {name}: {detail}", flush=True)

    def require(
        self,
        name: str,
        condition: bool,
        detail: str,
        failure_detail: str,
    ) -> None:
        self.add(name, "pass" if condition else "fail", detail if condition else failure_detail)
        if not condition:
            raise HilError(f"{name}: {failure_detail}")

    def finish(self) -> None:
        self.finished_at = utc_now()

    def serializable(self) -> dict[str, Any]:
        data = asdict(self)
        outcomes = [check.outcome for check in self.checks]
        data["summary"] = {
            "passed": outcomes.count("pass"),
            "failed": outcomes.count("fail"),
            "skipped": outcomes.count("skip"),
            "success": "fail" not in outcomes,
        }
        return data


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def expected_object_identifiers(device_instance: int) -> list[str]:
    """Return the exact Object_List order, including compatibility-stable indexes."""
    return [
        f"device,{device_instance}",
        *(f"binary-input,{instance}" for instance in PHYSICAL_INPUT_INSTANCES),
        *(f"analog-value,{instance}" for instance in COMPAT_ANALOG_VALUE_INSTANCES),
        "network-port,1",
        *(f"binary-input,{instance}" for instance in STATUS_INPUT_INSTANCES),
        *(f"analog-value,{instance}" for instance in APPENDED_ANALOG_VALUE_INSTANCES),
    ]


def error_signature(error: BaseException) -> tuple[str, str]:
    return (
        str(getattr(error, "errorClass", "")).replace("_", "-").lower(),
        str(getattr(error, "errorCode", "")).replace("_", "-").lower(),
    )


def revision_matches(expected: str | None, reported: object) -> bool:
    """Match a full or abbreviated revision embedded in a reported string."""
    if expected is None:
        return True
    expected_lower = expected.lower()
    reported_lower = str(reported).lower()
    matches = re.finditer(
        r"(?<![0-9a-f])[0-9a-f]{7,40}(?![0-9a-f])",
        reported_lower,
    )
    revisions = [
        match.group(0)
        for match in matches
        if not reported_lower[match.end() :].startswith("-dirty")
    ]
    return any(
        revision.startswith(expected_lower) or expected_lower.startswith(revision)
        for revision in revisions
    )


def hardware_profile_valid(value: object) -> bool:
    """Require exact field wiring metadata for the tested board/profile."""
    return value == EXPECTED_HARDWARE_PROFILE


def firmware_diagnostics_valid(status: dict[str, Any]) -> bool:
    firmware = status.get("firmware")
    if not isinstance(firmware, dict):
        return False
    running = firmware.get("running_partition")
    boot = firmware.get("boot_partition")
    update = firmware.get("next_update_partition")
    if not all(isinstance(item, dict) for item in (running, boot, update)):
        return False
    assert isinstance(running, dict) and isinstance(boot, dict)
    assert isinstance(update, dict)
    elf_sha = firmware.get("elf_sha256")
    return (
        isinstance(firmware.get("build_date"), str)
        and bool(firmware["build_date"])
        and isinstance(firmware.get("build_time"), str)
        and bool(firmware["build_time"])
        and isinstance(firmware.get("secure_version"), int)
        and firmware["secure_version"] >= 0
        and isinstance(elf_sha, str)
        and re.fullmatch(r"[0-9a-f]{64}", elf_sha) is not None
        and firmware.get("rollback_enabled") is True
        and running.get("label") == status.get("partition")
        and running.get("state") == status.get("state")
        and running.get("image_sha256") == status.get("image_sha256")
        and isinstance(running.get("address"), int)
        and running["address"] > 0
        and isinstance(running.get("size_bytes"), int)
        and running["size_bytes"] > 0
        and boot.get("matches_running") is True
        and boot.get("label") == running.get("label")
        and boot.get("address") == running.get("address")
        and boot.get("size_bytes") == running.get("size_bytes")
        and update.get("available") is True
        and isinstance(update.get("label"), str)
        and update.get("label") != running.get("label")
        and isinstance(update.get("address"), int)
        and update["address"] > 0
        and update.get("address") != running.get("address")
        and update.get("size_bytes") == running.get("size_bytes")
    )


def runtime_diagnostics_valid(status: dict[str, Any]) -> bool:
    system = status.get("system")
    fault_log = status.get("fault_log")
    fault_health = status.get("fault_log_health")
    if (
        not isinstance(system, dict)
        or not isinstance(fault_log, list)
        or not isinstance(fault_health, dict)
    ):
        return False
    temperature = system.get("temperature")
    if not isinstance(temperature, dict):
        return False
    current = temperature.get("current_c")
    minimum = temperature.get("minimum_c")
    maximum = temperature.get("maximum_c")
    last_result = temperature.get("last_result")
    uptime = system.get("uptime_ms")
    last_sample = temperature.get("last_sample_uptime_ms")
    sample_age = temperature.get("sample_age_ms")
    sequences = [
        event.get("sequence")
        for event in fault_log
        if isinstance(event, dict)
    ]
    temperature_ok = (
        temperature.get("valid") is True
        and isinstance(current, (int, float))
        and not isinstance(current, bool)
        and current == system.get("chip_temperature_c")
        and isinstance(minimum, (int, float))
        and isinstance(maximum, (int, float))
        and minimum <= current <= maximum
        and isinstance(temperature.get("sample_count_since_boot"), int)
        and temperature["sample_count_since_boot"] > 0
        and temperature.get("error_count_since_boot") == 0
        and temperature.get("sample_interval_ms") == 1000
        and isinstance(uptime, int)
        and isinstance(last_sample, int)
        and isinstance(sample_age, int)
        and 0 <= last_sample <= uptime
        and sample_age == uptime - last_sample
        and sample_age <= 2000
        and last_result == {"code": 0, "name": "ESP_OK"}
    )
    fault_ok = (
        len(sequences) == len(fault_log)
        and all(isinstance(sequence, int) and sequence > 0 for sequence in sequences)
        and sequences == sorted(sequences)
        and fault_health.get("capacity") == 16
        and fault_health.get("count") == len(fault_log)
        and isinstance(fault_health.get("total_event_count"), int)
        and fault_health["total_event_count"] >= len(fault_log)
        and fault_health.get("overwritten_event_count")
        == fault_health["total_event_count"] - len(fault_log)
        and fault_health.get("persistence_ready") is True
        and fault_health.get("write_failure_count_since_boot") == 0
        and fault_health.get("last_write_error")
        == {"code": 0, "name": "ESP_OK"}
        and fault_health.get("oldest_sequence")
        == (sequences[0] if sequences else None)
        and fault_health.get("newest_sequence")
        == (sequences[-1] if sequences else None)
    )
    return temperature_ok and fault_ok


def validate_args(args: argparse.Namespace) -> None:
    for label in ("device_instance", "client_instance"):
        value = getattr(args, label)
        if not 0 <= value <= MAX_DEVICE_INSTANCE:
            raise HilError(f"{label.replace('_', ' ')} must be 0-{MAX_DEVICE_INSTANCE}")
    if args.client_instance == args.device_instance:
        raise HilError("client and target Device instances must differ")
    try:
        local = ipaddress.ip_interface(args.local_address)
    except ValueError as error:
        raise HilError(f"invalid --local-address: {error}") from error
    try:
        target = ipaddress.ip_address(args.device_address)
    except ValueError as error:
        raise HilError(f"invalid --device-address: {error}") from error
    if local.version != 4 or local.network.prefixlen == 32:
        raise HilError("--local-address must be an IPv4 interface with a subnet prefix")
    if target.version != 4 or target not in local.network:
        raise HilError("target must be on the --local-address IPv4 subnet")
    if not 1 <= args.bacnet_port <= 65535:
        raise HilError("BACnet port must be 1-65535")
    if args.timeout <= 0:
        raise HilError("timeout must be greater than zero")
    if args.expected_source and not re.fullmatch(r"[0-9a-fA-F]{7,40}", args.expected_source):
        raise HilError("--expected-source must be a 7-40 digit hexadecimal revision")
    if args.expected_image_sha256 and not re.fullmatch(
        r"[0-9a-fA-F]{64}", args.expected_image_sha256
    ):
        raise HilError("--expected-image-sha256 must contain 64 hexadecimal digits")
    if args.mdns_hostname is not None and not re.fullmatch(
        r"(?=.{1,63}$)[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?",
        args.mdns_hostname,
    ):
        raise HilError("--mdns-hostname must be a 1-63 character RFC 1123 label")
    if args.token_file is not None and not args.certificate.is_file():
        raise HilError(f"pinned certificate not found: {args.certificate}")
    if args.token_file is not None and not args.token_file.is_file():
        raise HilError(f"token file not found: {args.token_file}")


def build_parser() -> argparse.ArgumentParser:
    project_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--local-address",
        required=True,
        help="host IPv4 interface in address/prefix form, for example 192.168.75.191/24",
    )
    parser.add_argument("--device-address", required=True, help="controller IPv4 address")
    parser.add_argument("--device-instance", required=True, type=int)
    parser.add_argument("--bacnet-port", type=int, default=47808)
    parser.add_argument(
        "--client-instance",
        type=int,
        default=4_194_000 + (os.getpid() % 300),
        help="temporary local BACnet Device instance",
    )
    parser.add_argument("--timeout", type=float, default=4.0)
    parser.add_argument("--report", type=Path, help="optional machine-readable JSON report")
    parser.add_argument(
        "--expect-inputs-off",
        action="store_true",
        help="require GPIO20, GPIO21, and GPIO22 to be inactive",
    )
    parser.add_argument(
        "--skip-capacity-test",
        action="store_true",
        help="skip the eight-subscription COV capacity boundary test",
    )
    parser.add_argument("--expected-version", help="required running firmware version")
    parser.add_argument("--expected-source", help="required running Git revision or prefix")
    parser.add_argument(
        "--expected-image-sha256", help="required running ESP image SHA-256"
    )
    parser.add_argument(
        "--token-file",
        type=Path,
        help="enable authenticated HTTPS checks with this ignored bearer-token file",
    )
    parser.add_argument(
        "--certificate",
        type=Path,
        default=project_root / "main" / "ota_server_cert.pem",
        help="pinned device certificate for HTTPS checks",
    )
    parser.add_argument(
        "--mdns-hostname",
        help="enable raw mDNS/DNS-SD validation for this .local hostname",
    )
    return parser


def load_runtime() -> dict[str, Any]:
    try:
        from bacpypes3.app import Application
        from bacpypes3.basetypes import BinaryPV, ErrorType
        from bacpypes3.pdu import Address
        from bacpypes3.primitivedata import ObjectIdentifier
    except ImportError as error:
        raise HilError(
            "bacpypes3 is required; install with "
            "python -m pip install -r requirements-hil.txt"
        ) from error
    return {
        "Application": Application,
        "Address": Address,
        "BinaryPV": BinaryPV,
        "ErrorType": ErrorType,
        "ObjectIdentifier": ObjectIdentifier,
    }


class HilRunner:
    def __init__(self, args: argparse.Namespace, report: TestReport, runtime: dict[str, Any]):
        self.args = args
        self.report = report
        self.runtime = runtime
        self.destination = runtime["Address"](args.device_address)
        self.app: Any = None
        self.objects: list[str] = []
        self.database_revision: int | None = None
        self.device_name: str | None = None
        self.vendor_identifier: int | None = None
        self.firmware_version: str | None = None

    async def operation(self, name: str, awaitable: Awaitable[Any]) -> Any:
        started = time.monotonic()
        try:
            value = await asyncio.wait_for(awaitable, timeout=self.args.timeout)
        except (KeyboardInterrupt, SystemExit):
            raise
        except BaseException as error:
            self.report.add(
                name,
                "fail",
                f"{type(error).__name__}: {error}",
                time.monotonic() - started,
            )
            raise HilError(f"{name} failed") from error
        self.report.add(name, "pass", "response received", time.monotonic() - started)
        return value

    async def read(self, obj: str, prop: str, index: int | None = None) -> Any:
        return await asyncio.wait_for(
            self.app.read_property(self.destination, obj, prop, index),
            timeout=self.args.timeout,
        )

    async def expect_error(
        self,
        name: str,
        awaitable: Awaitable[Any],
        accepted: set[tuple[str, str]],
        accepted_text: set[str] | None = None,
    ) -> None:
        started = time.monotonic()
        try:
            result = await asyncio.wait_for(awaitable, timeout=self.args.timeout)
        except (KeyboardInterrupt, SystemExit):
            raise
        except BaseException as error:
            signature = error_signature(error)
            text = str(error).replace("_", "-").lower()
            if signature in accepted or any(
                fragment in text for fragment in (accepted_text or set())
            ):
                detail = ":".join(signature) if any(signature) else text
                self.report.add(name, "pass", f"rejected with {detail}", time.monotonic() - started)
                return
            self.report.add(
                name,
                "fail",
                f"wrong error {signature[0]}:{signature[1]} ({error})",
                time.monotonic() - started,
            )
            raise HilError(f"{name} returned the wrong BACnet error") from error
        self.report.add(
            name,
            "fail",
            f"request unexpectedly succeeded: {result}",
            time.monotonic() - started,
        )
        raise HilError(f"{name} unexpectedly succeeded")

    async def run(self) -> None:
        app_args = argparse.Namespace(
            vendoridentifier=999,
            instance=self.args.client_instance,
            name="ESP32-P4 HIL Acceptance Client",
            address=self.args.local_address,
            network=0,
            foreign=None,
            ttl=30,
            bbmd=None,
        )
        self.app = self.runtime["Application"].from_args(app_args)
        try:
            await self.test_discovery()
            await self.test_object_model()
            if self.args.mdns_hostname is None:
                self.report.add(
                    "mDNS/DNS-SD discovery",
                    "skip",
                    "provide --mdns-hostname to enable the multicast discovery check",
                )
            else:
                await asyncio.to_thread(self.test_mdns)
            await self.test_rpm()
            await self.test_who_has()
            await self.test_cov()
            if self.args.skip_capacity_test:
                self.report.add("COV capacity boundary", "skip", "disabled by command line")
            else:
                await self.test_cov_capacity()
            await self.test_negative_access()
            if self.args.token_file is None:
                self.report.add(
                    "Authenticated HTTPS health",
                    "skip",
                    "provide --token-file to enable the protected management checks",
                )
            else:
                await asyncio.to_thread(self.test_https)
        finally:
            if self.app is not None:
                self.app.close()

    async def test_discovery(self) -> None:
        directed = await self.operation(
            "Directed Who-Is/I-Am",
            self.app.who_is(
                self.args.device_instance,
                self.args.device_instance,
                address=self.destination,
                timeout=self.args.timeout,
            ),
        )
        broadcast = await self.operation(
            "Broadcast Who-Is/I-Am",
            self.app.who_is(
                self.args.device_instance,
                self.args.device_instance,
                timeout=self.args.timeout,
            ),
        )
        for label, responses in (("Directed", directed), ("Broadcast", broadcast)):
            matches = [
                response
                for response in responses
                if int(response.iAmDeviceIdentifier[1]) == self.args.device_instance
            ]
            self.report.require(
                f"{label} discovery identity",
                len(matches) == 1,
                "one matching I-Am received",
                f"expected one matching I-Am, received {len(matches)}",
            )
            if matches:
                response = matches[0]
                self.report.require(
                    f"{label} I-Am capabilities",
                    int(response.maxAPDULengthAccepted) == 1476
                    and str(response.segmentationSupported) == "no-segmentation",
                    f"APDU=1476, segmentation={response.segmentationSupported}",
                    "I-Am capability fields are unexpected",
                )

    async def test_object_model(self) -> None:
        device = f"device,{self.args.device_instance}"
        count = int(await self.read(device, "object-list", 0))
        values = list(await self.read(device, "object-list"))
        self.objects = [str(value) for value in values]
        expected = expected_object_identifiers(self.args.device_instance)
        self.report.require(
            "Complete ordered Object_List",
            count == len(self.objects) == len(expected) and self.objects == expected,
            f"all {len(expected)} objects present at compatibility-stable indexes",
            f"expected={expected}; received={self.objects}",
        )

        metadata = {
            prop: await self.read(device, prop)
            for prop in (
                "object-name",
                "system-status",
                "vendor-name",
                "vendor-identifier",
                "model-name",
                "firmware-revision",
                "application-software-version",
                "protocol-revision",
                "protocol-services-supported",
                "protocol-object-types-supported",
                "max-apdu-length-accepted",
                "segmentation-supported",
                "database-revision",
                "description",
                "location",
            )
        }
        self.database_revision = int(metadata["database-revision"])
        self.device_name = str(metadata["object-name"])
        self.vendor_identifier = int(metadata["vendor-identifier"])
        self.firmware_version = str(metadata["firmware-revision"])
        strings_ok = all(
            str(metadata[prop]).strip()
            for prop in (
                "object-name",
                "vendor-name",
                "model-name",
                "firmware-revision",
                "application-software-version",
                "description",
                "location",
            )
        )
        version_ok = (
            self.args.expected_version is None
            or str(metadata["firmware-revision"]) == self.args.expected_version
        )
        source_ok = revision_matches(
            self.args.expected_source,
            metadata["application-software-version"],
        )
        self.report.require(
            "Device identity metadata",
            strings_ok
            and version_ok
            and source_ok
            and str(metadata["system-status"]) == "operational-read-only",
            f"name={metadata['object-name']!s}; firmware={metadata['application-software-version']!s}",
            "identity, firmware revision, source revision, or system status is unexpected",
        )
        service_text = str(metadata["protocol-services-supported"])
        type_text = str(metadata["protocol-object-types-supported"])
        required_services = {
            "subscribe-cov",
            "read-property",
            "read-property-multiple",
            "i-am",
            "i-have",
            "who-has",
            "who-is",
        }
        required_types = {"analog-value", "binary-input", "device", "network-port"}
        self.report.require(
            "Advertised protocol capabilities",
            int(metadata["protocol-revision"]) == 17
            and int(metadata["max-apdu-length-accepted"]) == 1476
            and str(metadata["segmentation-supported"]) == "no-segmentation"
            and required_services.issubset(set(service_text.split(";")))
            and required_types.issubset(set(type_text.split(";"))),
            "protocol revision 17 and implemented services/object types advertised",
            "Device protocol metadata does not match the implementation",
        )

        names: list[str] = [str(metadata["object-name"])]
        physical_values: dict[int, str] = {}
        for instance in PHYSICAL_INPUT_INSTANCES:
            obj = f"binary-input,{instance}"
            name = str(await self.read(obj, "object-name"))
            present = str(await self.read(obj, "present-value"))
            reliability = str(await self.read(obj, "reliability"))
            polarity = str(await self.read(obj, "polarity"))
            names.append(name)
            physical_values[instance] = present
            self.report.require(
                f"GPIO{instance} Binary Input",
                bool(name.strip())
                and present in {"active", "inactive"}
                and reliability in {"no-fault-detected", "unreliable-other"}
                and polarity in {"normal", "reverse"},
                f"{name!r}; {present}; {polarity}; {reliability}",
                "metadata, value, polarity, or reliability is invalid",
            )
        if self.args.expect_inputs_off:
            self.report.require(
                "Disconnected physical inputs",
                set(physical_values.values()) == {"inactive"},
                "GPIO20/GPIO21/GPIO22 are inactive",
                f"unexpected values: {physical_values}",
            )

        expected_status = {
            1001: ("Status Ethernet Link", "active"),
            1002: ("Status IPv4 Assigned", "active"),
        }
        for instance, (expected_name, expected_value) in expected_status.items():
            obj = f"binary-input,{instance}"
            name = str(await self.read(obj, "object-name"))
            names.append(name)
            present = str(await self.read(obj, "present-value"))
            reliability = str(await self.read(obj, "reliability"))
            self.report.require(
                f"{expected_name} Binary Input",
                name == expected_name
                and present == expected_value
                and reliability == "no-fault-detected",
                f"{present}; {reliability}",
                f"expected active/no-fault-detected, received {present}/{reliability}",
            )

        for instance in ANALOG_VALUE_INSTANCES:
            obj = f"analog-value,{instance}"
            name = str(await self.read(obj, "object-name"))
            value = float(await self.read(obj, "present-value"))
            reliability = str(await self.read(obj, "reliability"))
            names.append(name)
            self.report.require(
                f"{obj} diagnostic",
                bool(name.strip()) and value >= 0 and reliability == "no-fault-detected",
                f"{name!r}={value:g}; {reliability}",
                f"invalid diagnostic {name!r}={value}; {reliability}",
            )

        network_name = str(await self.read("network-port,1", "object-name"))
        names.append(network_name)
        network_address = bytes(await self.read("network-port,1", "ip-address"))
        self.report.require(
            "Network Port live state",
            network_name == "BACnet/IP Ethernet"
            and network_address == ipaddress.ip_address(self.args.device_address).packed
            and int(await self.read("network-port,1", "bacnet-ip-udp-port"))
            == self.args.bacnet_port
            and float(await self.read("network-port,1", "link-speed")) > 0
            and str(await self.read("network-port,1", "reliability"))
            == "no-fault-detected",
            f"IPv4={self.args.device_address}; UDP={self.args.bacnet_port}; link reliable",
            "Network Port address, UDP port, speed, or reliability is unexpected",
        )
        self.report.require(
            "Unique BACnet Object_Name values",
            len(names) == len(set(names)),
            f"all {len(names)} object names are unique",
            "one or more Object_Name values are duplicated",
        )

    def test_mdns(self) -> None:
        try:
            import mdns_probe
        except ImportError as error:
            raise HilError("tools/mdns_probe.py is unavailable") from error
        if (
            self.device_name is None
            or self.vendor_identifier is None
            or self.firmware_version is None
        ):
            raise HilError("BACnet identity metadata is unavailable for mDNS validation")
        interface = str(ipaddress.ip_interface(self.args.local_address).ip)
        started = time.monotonic()
        try:
            records = mdns_probe.probe(
                interface,
                self.args.mdns_hostname,
                self.args.device_address,
                self.device_name,
                443,
                self.args.bacnet_port,
                self.args.device_instance,
                self.vendor_identifier,
                self.firmware_version,
                self.args.timeout,
            )
        except (mdns_probe.MdnsProbeError, OSError) as error:
            self.report.add(
                "mDNS/DNS-SD discovery",
                "fail",
                str(error),
                time.monotonic() - started,
            )
            raise HilError("mDNS/DNS-SD discovery failed") from error
        self.report.add(
            "mDNS/DNS-SD discovery",
            "pass",
            f"{self.args.mdns_hostname}.local and HTTPS/BACnet services verified ({len(records)} records)",
            time.monotonic() - started,
        )

    async def test_rpm(self) -> None:
        error_type = self.runtime["ErrorType"]
        property_count = 0
        errors: list[str] = []
        for obj in self.objects:
            values = await asyncio.wait_for(
                self.app.read_property_multiple(
                    self.destination,
                    [self.runtime["ObjectIdentifier"](obj), ["all"]],
                ),
                timeout=self.args.timeout,
            )
            property_count += len(values)
            errors.extend(
                f"{item[0]}:{item[1]}={item[3]}"
                for item in values
                if isinstance(item[3], error_type)
            )
        self.report.require(
            "ReadPropertyMultiple All",
            property_count >= 200 and not errors,
            f"{property_count} property results across all objects; zero errors",
            f"{property_count} results; errors={errors}",
        )

    async def test_who_has(self) -> None:
        object_identifier = self.runtime["ObjectIdentifier"]
        for instance in STATUS_INPUT_INSTANCES:
            replies = await self.operation(
                f"Who-Has binary-input,{instance}",
                self.app.who_has(
                    object_identifier=object_identifier(f"binary-input,{instance}"),
                    address=self.destination,
                    timeout=self.args.timeout,
                ),
            )
            self.report.require(
                f"I-Have binary-input,{instance}",
                len(replies) == 1,
                "one matching I-Have received",
                f"expected one I-Have, received {len(replies)}",
            )

    async def next_cov_value(self, subscription: Any, expected: str) -> None:
        prop, value = await asyncio.wait_for(
            subscription.get_value(), timeout=self.args.timeout
        )
        self.report.require(
            f"COV {subscription.monitored_object_identifier}",
            str(prop) == "present-value" and str(value) == expected,
            f"Present_Value={expected}",
            f"unexpected notification {prop}={value}",
        )

    async def test_cov(self) -> None:
        object_identifier = self.runtime["ObjectIdentifier"]
        for confirmed in (True, False):
            mode = "confirmed" if confirmed else "unconfirmed"
            for instance in STATUS_INPUT_INSTANCES:
                obj = object_identifier(f"binary-input,{instance}")
                async with self.app.change_of_value(
                    self.destination,
                    obj,
                    issue_confirmed_notifications=confirmed,
                    lifetime=30,
                ) as subscription:
                    await self.next_cov_value(subscription, "active")
                self.report.add(
                    f"{mode.title()} COV binary-input,{instance}",
                    "pass",
                    "initial notification received and subscription cancelled",
                )

    async def test_cov_capacity(self) -> None:
        object_identifier = self.runtime["ObjectIdentifier"]
        all_inputs = (*PHYSICAL_INPUT_INSTANCES, *STATUS_INPUT_INSTANCES)
        async with AsyncExitStack() as stack:
            subscriptions: list[Any] = []
            for offset in range(COV_CAPACITY):
                instance = all_inputs[offset % len(all_inputs)]
                subscription = await stack.enter_async_context(
                    self.app.change_of_value(
                        self.destination,
                        object_identifier(f"binary-input,{instance}"),
                        subscriber_process_identifier=41_000 + offset,
                        issue_confirmed_notifications=bool(offset % 2),
                        lifetime=30,
                    )
                )
                subscriptions.append(subscription)
                await asyncio.wait_for(subscription.get_value(), timeout=self.args.timeout)
            overflow = self.app.change_of_value(
                self.destination,
                object_identifier("binary-input,20"),
                subscriber_process_identifier=41_999,
                issue_confirmed_notifications=True,
                lifetime=30,
            )
            try:
                await asyncio.wait_for(overflow.__aenter__(), timeout=self.args.timeout)
            except (KeyboardInterrupt, SystemExit):
                raise
            except BaseException as error:
                self.report.require(
                    "COV ninth-subscription rejection",
                    error_signature(error) == ("resources", "no-space-to-add-list-element"),
                    "rejected with resources:no-space-to-add-list-element",
                    f"wrong BACnet error {error_signature(error)} ({error})",
                )
            else:
                await overflow.__aexit__(None, None, None)
                self.report.require(
                    "COV ninth-subscription rejection",
                    False,
                    "",
                    "ninth subscription unexpectedly succeeded",
                )
            active = float(await self.read("analog-value,1009", "present-value"))
            self.report.require(
                "COV capacity preserved after rejection",
                active == COV_CAPACITY,
                "all eight existing subscriptions remain active",
                f"active subscription diagnostic is {active:g}",
            )
            for subscription in subscriptions:
                await asyncio.wait_for(
                    subscription.refresh_subscription(), timeout=self.args.timeout
                )
            self.report.add(
                "COV existing-subscription renewals",
                "pass",
                "all eight original identities renewed; none was evicted",
            )
        await asyncio.sleep(0.15)
        active = float(await self.read("analog-value,1009", "present-value"))
        self.report.require(
            "COV capacity cleanup",
            active == 0,
            "all eight temporary subscriptions were cancelled",
            f"active subscription diagnostic is {active:g}",
        )

    async def test_negative_access(self) -> None:
        object_identifier = self.runtime["ObjectIdentifier"]
        binary_pv = self.runtime["BinaryPV"]
        await self.expect_error(
            "Read-only Binary Input",
            self.app.write_property(
                self.destination,
                object_identifier("binary-input,1001"),
                "present-value",
                binary_pv.inactive,
            ),
            set(),
            {"unrecognized-service", "write-access-denied"},
        )
        await self.expect_error(
            "Unknown object rejection",
            self.app.read_property(
                self.destination, "binary-input,4194302", "present-value"
            ),
            {("object", "unknown-object")},
        )
        await self.expect_error(
            "Invalid Object_List index rejection",
            self.app.read_property(
                self.destination,
                f"device,{self.args.device_instance}",
                "object-list",
                9999,
            ),
            {("property", "invalid-array-index")},
        )

    def test_https(self) -> None:
        try:
            import ota_client
        except ImportError as error:
            raise HilError("tools/ota_client.py is unavailable") from error
        token = ota_client._read_token(self.args.token_file)
        status = ota_client._fetch_status(
            self.args.device_address,
            443,
            self.args.certificate,
            self.args.timeout,
            token,
        )
        source = str(status.get("git_revision", "")).lower()
        image_sha = str(status.get("image_sha256", "")).lower()
        network = status.get("network", {})
        system = status.get("system", {})
        configuration = status.get("configuration", {})
        discovery = status.get("discovery", {})
        hardware = status.get("hardware", {})
        bacnet = status.get("bacnet", {})
        watchdog = system.get("task_watchdog", {}) if isinstance(system, dict) else {}
        gpio_diagnostics = status.get("gpio_diagnostics", [])
        signal_diagnostics_ok = isinstance(gpio_diagnostics, list) and len(
            gpio_diagnostics
        ) == len(PHYSICAL_INPUT_INSTANCES)
        if signal_diagnostics_ok:
            for item in gpio_diagnostics:
                signal = item.get("signal", {}) if isinstance(item, dict) else {}
                signal_diagnostics_ok = signal_diagnostics_ok and (
                    isinstance(signal, dict)
                    and signal.get("accepted_transition_count")
                    == item.get("transition_count")
                    and all(
                        isinstance(signal.get(key), int) and signal[key] >= 0
                        for key in (
                            "raw_edge_count",
                            "accepted_transition_count",
                            "rejected_pulse_count",
                            "chatter_event_count",
                            "candidate_age_ms",
                            "last_raw_edge_uptime_ms",
                            "last_rejected_pulse_uptime_ms",
                            "last_rejected_pulse_width_ms",
                        )
                    )
                    and isinstance(signal.get("chattering"), bool)
                    and isinstance(signal.get("candidate_active"), bool)
                    and isinstance(signal.get("candidate_level"), bool)
                )
        version_ok = (
            self.args.expected_version is None
            or status.get("version") == self.args.expected_version
        )
        source_ok = revision_matches(self.args.expected_source, source)
        image_ok = (
            self.args.expected_image_sha256 is None
            or image_sha == self.args.expected_image_sha256.lower()
        )
        healthy = (
            status.get("project") == "esp32_p4_bacnet_switches"
            and status.get("state") == "valid"
            and version_ok
            and source_ok
            and image_ok
            and isinstance(network, dict)
            and network.get("ipv4") == self.args.device_address
            and network.get("link_up") is True
            and network.get("speed_mbps") in {10, 100}
            and isinstance(configuration, dict)
            and configuration.get("restart_required") is False
            and configuration.get("active_database_revision")
            == configuration.get("saved_database_revision")
            and self.database_revision
            == configuration.get("active_database_revision")
            + FIRMWARE_DATABASE_REVISION_OFFSET
            and isinstance(bacnet, dict)
            and bacnet.get("active_cov_subscriptions") == 0
            and bacnet.get("cov_timeouts") == 0
            and isinstance(watchdog, dict)
            and all(
                isinstance(item, dict) and item.get("healthy") is True
                for item in watchdog.values()
            )
            and signal_diagnostics_ok
            and hardware_profile_valid(hardware)
            and firmware_diagnostics_valid(status)
            and runtime_diagnostics_valid(status)
            and (
                self.args.mdns_hostname is None
                or (
                    isinstance(discovery, dict)
                    and discovery.get("mdns_ready") is True
                    and discovery.get("hostname") == self.args.mdns_hostname
                    and discovery.get("local_fqdn")
                    == f"{self.args.mdns_hostname}.local"
                    and discovery.get("hostname_conflict_count") == 0
                    and discovery.get("last_error")
                    == {"code": 0, "name": "ESP_OK"}
                    and discovery.get("services")
                    == {
                        "https": {"advertised": True, "port": 443},
                        "bacnet": {
                            "advertised": True,
                            "port": self.args.bacnet_port,
                        },
                    }
                )
            )
        )
        self.report.require(
            "Authenticated HTTPS health",
            healthy,
            f"version={status.get('version')}; source={source}; image={image_sha}; firmware/runtime diagnostics healthy",
            "firmware identity, OTA state, network, configuration, COV cleanup, watchdog, wiring, image, temperature, or fault-log health failed",
        )
        certificate_der = ota_client._certificate_der(self.args.certificate)
        fingerprint = hashlib.sha256(certificate_der).hexdigest()
        for method, path in (
            ("GET", "/ota/status"),
            ("GET", "/config"),
            ("GET", "/network/config"),
            ("POST", "/diagnostics/input-self-test"),
            ("POST", "/system/reboot"),
        ):
            connection = ota_client._connection(
                self.args.device_address,
                443,
                self.args.certificate,
                self.args.timeout,
            )
            try:
                connection.request(method, path, headers={"Accept": "application/json"})
                response = connection.getresponse()
                response.read(MAX_HTTP_RESPONSE_BYTES + 1)
                if response.status != 401:
                    raise HilError(
                        f"unauthenticated {method} {path} returned HTTP {response.status}"
                    )
            finally:
                connection.close()
        self.report.add(
            "HTTPS authentication boundary",
            "pass",
            f"five protected routes returned 401; certificate SHA-256={fingerprint}",
        )


def write_report(path: Path, report: TestReport) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(report.serializable(), indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


async def async_main(args: argparse.Namespace, report: TestReport) -> None:
    runtime = load_runtime()
    report.bacpypes3_version = importlib.metadata.version("bacpypes3")
    if report.bacpypes3_version != REQUIRED_BACPYPES3_VERSION:
        report.add(
            "BACpypes3 version",
            "fail",
            f"found {report.bacpypes3_version}; expected {REQUIRED_BACPYPES3_VERSION}",
        )
        raise HilError("unsupported BACpypes3 version")
    report.add(
        "BACpypes3 version",
        "pass",
        f"using pinned version {report.bacpypes3_version}",
    )
    await HilRunner(args, report, runtime).run()


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    report = TestReport(
        started_at=utc_now(),
        target={
            "device_address": args.device_address,
            "device_instance": args.device_instance,
        },
        options={
            "local_address": args.local_address,
            "client_instance": args.client_instance,
            "bacnet_port": args.bacnet_port,
            "timeout": args.timeout,
            "expect_inputs_off": args.expect_inputs_off,
            "capacity_test": not args.skip_capacity_test,
            "authenticated_https": args.token_file is not None,
            "expected_version": args.expected_version,
            "expected_source": args.expected_source,
            "expected_image_sha256": args.expected_image_sha256,
            "mdns_hostname": args.mdns_hostname,
        },
    )
    exit_code = 0
    try:
        validate_args(args)
        asyncio.run(async_main(args, report))
    except (KeyboardInterrupt, SystemExit):
        raise
    except HilError as error:
        print(f"error: {error}", file=sys.stderr)
        if not any(check.outcome == "fail" for check in report.checks):
            report.add("HIL runner", "fail", str(error))
        exit_code = 1
    except BaseException as error:
        print(f"error: unexpected HIL runner failure: {error}", file=sys.stderr)
        report.add("HIL runner internal error", "fail", f"{type(error).__name__}: {error}")
        exit_code = 1
    finally:
        report.finish()
        if args.report:
            try:
                write_report(args.report, report)
                print(f"Report: {args.report}", flush=True)
            except OSError as error:
                print(f"error: cannot write report {args.report}: {error}", file=sys.stderr)
                exit_code = 1
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
