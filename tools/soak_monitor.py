#!/usr/bin/env python3
"""Continuously monitor ESP32-P4 health over pinned HTTPS and BACnet/IP."""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
import hashlib
import http.client
import ipaddress
import json
import math
import os
from pathlib import Path
import socket
import ssl
import statistics
import sys
import time
from typing import Any, Iterator

try:
    from tools import ota_client
except ImportError:  # Direct execution adds tools/, rather than the project root.
    import ota_client


WHO_IS_UNICAST = bytes.fromhex("81 0a 00 08 01 00 10 08")
MAX_RESPONSE_BYTES = 1024 * 1024
MONOTONIC_BACNET_COUNTERS = (
    "rx",
    "responses",
    "who_is",
    "who_has",
    "read_property",
    "read_property_multiple",
    "subscribe_cov",
    "cov_sent",
    "cov_acked",
    "ignored",
)
ERROR_BACNET_COUNTERS = ("errors", "malformed", "rate_limited", "cov_timeouts")
MONOTONIC_NETWORK_COUNTERS = (
    "link_up_count",
    "link_down_count",
    "reconnect_count",
    "ip_acquisition_count",
    "ip_changed_count",
)


class SoakError(RuntimeError):
    """An expected monitor, transport, or device-health failure."""


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace(
        "+00:00", "Z"
    )


def canonical_fingerprint(*values: object) -> str:
    encoded = json.dumps(
        values, separators=(",", ":"), sort_keys=True, ensure_ascii=True
    ).encode("ascii")
    return hashlib.sha256(encoded).hexdigest()


def decode_tag(payload: bytes, offset: int) -> tuple[int, bytes, int]:
    if offset >= len(payload):
        raise SoakError("truncated BACnet application tag")
    header = payload[offset]
    tag_number = header >> 4
    if tag_number == 15 or header & 0x08:
        raise SoakError("unexpected extended or context BACnet tag")
    length = header & 0x07
    offset += 1
    if length == 5:
        if offset >= len(payload):
            raise SoakError("truncated BACnet tag length")
        length = payload[offset]
        offset += 1
        if length == 254:
            if offset + 2 > len(payload):
                raise SoakError("truncated BACnet 16-bit tag length")
            length = int.from_bytes(payload[offset : offset + 2], "big")
            offset += 2
        elif length == 255:
            if offset + 4 > len(payload):
                raise SoakError("truncated BACnet 32-bit tag length")
            length = int.from_bytes(payload[offset : offset + 4], "big")
            offset += 4
    end = offset + length
    if end > len(payload):
        raise SoakError("BACnet tag value exceeds packet")
    return tag_number, payload[offset:end], end


def parse_i_am(packet: bytes) -> dict[str, int]:
    if len(packet) < 12 or packet[0] != 0x81:
        raise SoakError("not a BACnet/IPv4 packet")
    if packet[1] not in {0x0A, 0x0B}:
        raise SoakError(f"unexpected BVLC function 0x{packet[1]:02x}")
    declared_length = int.from_bytes(packet[2:4], "big")
    if declared_length != len(packet):
        raise SoakError(
            f"BACnet packet length mismatch ({declared_length} != {len(packet)})"
        )
    offset = 4
    if offset + 2 > len(packet) or packet[offset] != 1:
        raise SoakError("invalid BACnet NPDU")
    control = packet[offset + 1]
    offset += 2
    if control & 0x20:
        if offset + 3 > len(packet):
            raise SoakError("truncated NPDU destination")
        address_length = packet[offset + 2]
        offset += 3 + address_length + 1
        if offset > len(packet):
            raise SoakError("truncated NPDU destination address")
    if control & 0x08:
        if offset + 3 > len(packet):
            raise SoakError("truncated NPDU source")
        address_length = packet[offset + 2]
        offset += 3 + address_length
        if offset > len(packet):
            raise SoakError("truncated NPDU source address")
    if control & 0x80:
        raise SoakError("received a network-layer message instead of I-Am")
    if offset + 2 > len(packet) or packet[offset] >> 4 != 1 or packet[offset + 1] != 0:
        raise SoakError("BACnet APDU is not an I-Am")
    offset += 2
    values: list[tuple[int, bytes]] = []
    while offset < len(packet):
        tag, value, offset = decode_tag(packet, offset)
        values.append((tag, value))
    if [tag for tag, _ in values] != [12, 2, 9, 2]:
        raise SoakError("I-Am has unexpected application tags")
    if len(values[0][1]) != 4:
        raise SoakError("I-Am Device identifier is not four bytes")
    object_identifier = int.from_bytes(values[0][1], "big")
    if object_identifier >> 22 != 8:
        raise SoakError("I-Am object identifier is not a Device")
    return {
        "device_instance": object_identifier & 0x3FFFFF,
        "max_apdu": int.from_bytes(values[1][1], "big"),
        "segmentation": int.from_bytes(values[2][1], "big"),
        "vendor_identifier": int.from_bytes(values[3][1], "big"),
        "bvlc_function": packet[1],
    }


def probe_bacnet(
    device_address: str,
    device_instance: int,
    port: int,
    timeout: float,
) -> tuple[dict[str, Any], float]:
    started = time.monotonic()
    deadline = started + timeout
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
            client.settimeout(timeout)
            client.sendto(WHO_IS_UNICAST, (device_address, port))
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise SoakError("BACnet I-Am timed out")
                client.settimeout(remaining)
                packet, source = client.recvfrom(2048)
                decoded = parse_i_am(packet)
                if decoded["device_instance"] != device_instance:
                    continue
                if source != (device_address, port):
                    raise SoakError(f"I-Am came from unexpected source {source}")
                decoded["source"] = f"{source[0]}:{source[1]}"
                return decoded, (time.monotonic() - started) * 1000.0
    except socket.timeout as error:
        raise SoakError("BACnet I-Am timed out") from error
    except OSError as error:
        raise SoakError(f"BACnet probe failed: {error}") from error


def fetch_json(
    device_address: str,
    certificate: Path,
    token: str,
    path: str,
    timeout: float,
) -> tuple[dict[str, Any], float]:
    started = time.monotonic()
    try:
        connection = ota_client._connection(device_address, 443, certificate, timeout)
    except (OSError, ssl.SSLError, http.client.HTTPException) as error:
        raise SoakError(f"{path} connection failed: {error}") from error
    try:
        connection.request(
            "GET",
            path,
            headers={
                "Authorization": f"Bearer {token}",
                "Accept": "application/json",
                "User-Agent": "esp32-p4-soak/1",
            },
        )
        response = connection.getresponse()
        payload = response.read(MAX_RESPONSE_BYTES + 1)
        if response.status != 200:
            raise SoakError(f"{path} returned HTTP {response.status}")
    except (OSError, ssl.SSLError, http.client.HTTPException) as error:
        raise SoakError(f"{path} request failed: {error}") from error
    finally:
        connection.close()
    if len(payload) > MAX_RESPONSE_BYTES:
        raise SoakError(f"{path} response exceeds 1 MiB")
    try:
        decoded = json.loads(payload)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise SoakError(f"{path} returned invalid JSON") from error
    if not isinstance(decoded, dict):
        raise SoakError(f"{path} did not return a JSON object")
    return decoded, (time.monotonic() - started) * 1000.0


@dataclass(frozen=True)
class Baseline:
    project: str
    version: str
    git_revision: str
    image_sha256: str
    partition: str
    boot_count: int
    reset_reason_code: int
    ipv4: str
    mac: str
    device_instance: int
    vendor_identifier: int
    bacnet_port: int
    configuration_sha256: str
    fault_log_sha256: str

    @classmethod
    def from_values(
        cls,
        status: dict[str, Any],
        config: dict[str, Any],
        network_config: dict[str, Any],
    ) -> "Baseline":
        try:
            system = status["system"]
            network = status["network"]
            reset_reason = system["reset_reason"]
            return cls(
                project=str(status["project"]),
                version=str(status["version"]),
                git_revision=str(status["git_revision"]),
                image_sha256=str(status["image_sha256"]),
                partition=str(status["partition"]),
                boot_count=int(system["boot_count"]),
                reset_reason_code=int(reset_reason["code"]),
                ipv4=str(network["ipv4"]),
                mac=str(network["mac"]),
                device_instance=int(config["device_instance"]),
                vendor_identifier=int(config["vendor_identifier"]),
                bacnet_port=int(config["bacnet_port"]),
                configuration_sha256=canonical_fingerprint(config, network_config),
                fault_log_sha256=canonical_fingerprint(status.get("fault_log", [])),
            )
        except (KeyError, TypeError, ValueError) as error:
            raise SoakError(f"baseline response is missing or malformed: {error}") from error


def nested_counter(container: dict[str, Any], section: str, key: str) -> int | None:
    value = container.get(section, {})
    if not isinstance(value, dict):
        return None
    try:
        return int(value[key])
    except (KeyError, TypeError, ValueError):
        return None


def evaluate_sample(
    baseline: Baseline,
    previous_status: dict[str, Any] | None,
    status: dict[str, Any],
    config: dict[str, Any],
    network_config: dict[str, Any],
    bacnet: dict[str, Any],
    *,
    minimum_heap_bytes: int,
    maximum_temperature_c: float,
) -> list[str]:
    alerts: list[str] = []
    try:
        system = status["system"]
        network = status["network"]
        configuration = status["configuration"]
        network_configuration = status["network_configuration"]
        bacnet_counters = status["bacnet"]
    except (KeyError, TypeError) as error:
        return [f"status-missing-section:{error}"]

    stable = {
        "project": baseline.project,
        "version": baseline.version,
        "git_revision": baseline.git_revision,
        "image_sha256": baseline.image_sha256,
        "partition": baseline.partition,
    }
    for key, expected in stable.items():
        if status.get(key) != expected:
            alerts.append(f"{key}-changed:{status.get(key)!r}!={expected!r}")
    if status.get("state") != "valid":
        alerts.append(f"ota-state:{status.get('state')!r}")
    if system.get("boot_count") != baseline.boot_count:
        alerts.append(f"boot-count-changed:{system.get('boot_count')!r}!={baseline.boot_count}")
    reset = system.get("reset_reason", {})
    if not isinstance(reset, dict) or reset.get("code") != baseline.reset_reason_code:
        alerts.append("reset-reason-changed")

    if network.get("ipv4") != baseline.ipv4 or network.get("mac") != baseline.mac:
        alerts.append("network-identity-changed")
    for key in ("link_up", "full_duplex", "autonegotiation"):
        if network.get(key) is not True:
            alerts.append(f"network-{key}-unhealthy:{network.get(key)!r}")
    if network.get("speed_mbps") not in {10, 100}:
        alerts.append(f"network-speed-invalid:{network.get('speed_mbps')!r}")

    watchdog = system.get("task_watchdog", {})
    if not isinstance(watchdog, dict) or not watchdog:
        alerts.append("task-watchdog-missing")
    else:
        for name, state in watchdog.items():
            if not isinstance(state, dict) or state.get("healthy") is not True:
                alerts.append(f"task-watchdog-{name}-unhealthy")
    try:
        free_heap = int(system["free_heap_bytes"])
        minimum_heap = int(system["minimum_free_heap_bytes"])
        temperature = float(system["chip_temperature_c"])
    except (KeyError, TypeError, ValueError):
        alerts.append("system-metrics-missing-or-invalid")
    else:
        if free_heap < minimum_heap_bytes:
            alerts.append(f"free-heap-below-floor:{free_heap}<{minimum_heap_bytes}")
        if minimum_heap < minimum_heap_bytes:
            alerts.append(f"minimum-heap-below-floor:{minimum_heap}<{minimum_heap_bytes}")
        if minimum_heap > free_heap:
            alerts.append(f"minimum-heap-exceeds-current:{minimum_heap}>{free_heap}")
        if not -20.0 <= temperature <= maximum_temperature_c:
            alerts.append(f"temperature-out-of-range:{temperature}")

    if configuration.get("active_database_revision") != configuration.get(
        "saved_database_revision"
    ) or configuration.get("restart_required") is not False:
        alerts.append("bacnet-configuration-pending")
    if network_configuration.get("active_revision") != network_configuration.get(
        "saved_revision"
    ) or network_configuration.get("restart_required") is not False:
        alerts.append("network-configuration-pending")
    if network_configuration.get("trial_active") is not False:
        alerts.append("network-configuration-trial-active")
    if canonical_fingerprint(config, network_config) != baseline.configuration_sha256:
        alerts.append("configuration-content-changed")
    if canonical_fingerprint(status.get("fault_log", [])) != baseline.fault_log_sha256:
        alerts.append("fault-log-changed")

    if bacnet.get("device_instance") != baseline.device_instance:
        alerts.append("bacnet-device-instance-changed")
    if bacnet.get("vendor_identifier") != baseline.vendor_identifier:
        alerts.append("bacnet-vendor-identifier-changed")
    if bacnet.get("max_apdu") != 1476 or bacnet.get("segmentation") != 3:
        alerts.append("bacnet-capabilities-invalid")
    if bacnet_counters.get("active_cov_subscriptions") != 0:
        alerts.append(
            f"unexpected-active-cov:{bacnet_counters.get('active_cov_subscriptions')!r}"
        )

    gpio = status.get("gpio_diagnostics")
    if not isinstance(gpio, list) or len(gpio) != 3:
        alerts.append("gpio-diagnostics-malformed")
    else:
        for item in gpio:
            if not isinstance(item, dict) or item.get("fault") is not False:
                alerts.append(f"gpio-fault:{item.get('gpio') if isinstance(item, dict) else '?'}")
                continue
            signal = item.get("signal")
            if not isinstance(signal, dict):
                alerts.append(f"gpio-signal-missing:{item.get('gpio')}")
            elif signal.get("chattering") is True:
                alerts.append(f"gpio-chattering:{item.get('gpio')}")

    if previous_status is not None:
        previous_system = previous_status.get("system", {})
        for key in ("uptime_ms", "free_heap_bytes", "minimum_free_heap_bytes"):
            if key not in system or key not in previous_system:
                alerts.append(f"system-{key}-missing")
        current_uptime = nested_counter(status, "system", "uptime_ms")
        previous_uptime = nested_counter(previous_status, "system", "uptime_ms")
        if current_uptime is not None and previous_uptime is not None:
            if current_uptime < previous_uptime:
                alerts.append("uptime-decreased")
        for key in MONOTONIC_BACNET_COUNTERS:
            current = nested_counter(status, "bacnet", key)
            previous = nested_counter(previous_status, "bacnet", key)
            if current is None or previous is None:
                alerts.append(f"bacnet-{key}-missing")
            elif current < previous:
                alerts.append(f"bacnet-{key}-decreased:{current}<{previous}")
        for key in ERROR_BACNET_COUNTERS:
            current = nested_counter(status, "bacnet", key)
            previous = nested_counter(previous_status, "bacnet", key)
            if current is None or previous is None:
                alerts.append(f"bacnet-{key}-missing")
            elif current > previous:
                alerts.append(f"bacnet-{key}-increased:{current}>{previous}")
        for key in MONOTONIC_NETWORK_COUNTERS:
            current = nested_counter(status, "network", key)
            previous = nested_counter(previous_status, "network", key)
            if current is None or previous is None:
                alerts.append(f"network-{key}-missing")
            elif current < previous:
                alerts.append(f"network-{key}-decreased:{current}<{previous}")
        for key in ("link_down_count", "reconnect_count", "ip_changed_count"):
            current = nested_counter(status, "network", key)
            previous = nested_counter(previous_status, "network", key)
            if current is not None and previous is not None and current > previous:
                alerts.append(f"network-{key}-increased:{current}>{previous}")
    return alerts


def sample_schedule(duration: float, interval: float) -> Iterator[float]:
    sample = 0.0
    while sample < duration:
        yield sample
        sample += interval
    yield duration


class JsonlLog:
    def __init__(self, path: Path):
        if not path.parent.is_dir():
            raise SoakError(f"output directory does not exist: {path.parent}")
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
        try:
            descriptor = os.open(path, flags, 0o600)
        except FileExistsError as error:
            raise SoakError(f"refusing to overwrite existing output: {path}") from error
        except OSError as error:
            raise SoakError(f"cannot create output {path}: {error}") from error
        self._file = os.fdopen(descriptor, "w", encoding="utf-8")
        self._rows = 0

    def write(self, value: object, *, sync: bool = False) -> None:
        self._file.write(json.dumps(value, separators=(",", ":"), sort_keys=True) + "\n")
        self._file.flush()
        self._rows += 1
        if sync or self._rows % 10 == 0:
            os.fsync(self._file.fileno())

    def close(self) -> None:
        self._file.close()


@dataclass
class MonitorStats:
    planned_duration_seconds: float
    started_at: str
    samples: int = 0
    successful_samples: int = 0
    request_failures: int = 0
    samples_with_alerts: int = 0
    alert_counts: Counter[str] = field(default_factory=Counter)
    https_latencies_ms: list[float] = field(default_factory=list)
    bacnet_latencies_ms: list[float] = field(default_factory=list)
    first_free_heap: int | None = None
    last_free_heap: int | None = None
    lowest_free_heap: int | None = None
    maximum_temperature_c: float | None = None

    def record_success(
        self,
        status: dict[str, Any],
        https_ms: float,
        bacnet_ms: float,
        alerts: list[str],
    ) -> None:
        self.samples += 1
        self.successful_samples += 1
        self.https_latencies_ms.append(https_ms)
        self.bacnet_latencies_ms.append(bacnet_ms)
        system = status["system"]
        free_heap = int(system["free_heap_bytes"])
        temperature = float(system["chip_temperature_c"])
        if self.first_free_heap is None:
            self.first_free_heap = free_heap
        self.last_free_heap = free_heap
        self.lowest_free_heap = (
            free_heap if self.lowest_free_heap is None else min(self.lowest_free_heap, free_heap)
        )
        self.maximum_temperature_c = (
            temperature
            if self.maximum_temperature_c is None
            else max(self.maximum_temperature_c, temperature)
        )
        if alerts:
            self.samples_with_alerts += 1
            self.alert_counts.update(alert.split(":", 1)[0] for alert in alerts)

    def record_failure(self, error: str) -> None:
        self.samples += 1
        self.request_failures += 1
        self.alert_counts.update([error.split(":", 1)[0]])

    @staticmethod
    def latency_summary(values: list[float]) -> dict[str, float] | None:
        if not values:
            return None
        return {
            "minimum": round(min(values), 3),
            "median": round(statistics.median(values), 3),
            "maximum": round(max(values), 3),
        }

    def summary(self, *, finished_at: str, elapsed_seconds: float, interrupted: bool) -> dict[str, Any]:
        return {
            "type": "summary",
            "started_at": self.started_at,
            "finished_at": finished_at,
            "planned_duration_seconds": self.planned_duration_seconds,
            "elapsed_seconds": round(elapsed_seconds, 3),
            "interrupted": interrupted,
            "samples": self.samples,
            "successful_samples": self.successful_samples,
            "request_failures": self.request_failures,
            "samples_with_alerts": self.samples_with_alerts,
            "alert_counts": dict(self.alert_counts),
            "https_latency_ms": self.latency_summary(self.https_latencies_ms),
            "bacnet_latency_ms": self.latency_summary(self.bacnet_latencies_ms),
            "first_free_heap_bytes": self.first_free_heap,
            "last_free_heap_bytes": self.last_free_heap,
            "heap_change_bytes": None
            if self.first_free_heap is None or self.last_free_heap is None
            else self.last_free_heap - self.first_free_heap,
            "lowest_free_heap_bytes": self.lowest_free_heap,
            "maximum_temperature_c": self.maximum_temperature_c,
            "success": not interrupted
            and self.request_failures == 0
            and self.samples_with_alerts == 0,
        }


def validate_args(args: argparse.Namespace) -> None:
    try:
        address = ipaddress.ip_address(args.device_address)
    except ValueError as error:
        raise SoakError(f"invalid device address: {error}") from error
    if address.version != 4:
        raise SoakError("device address must be IPv4")
    if not 0 <= args.device_instance <= 4_194_302:
        raise SoakError("device instance must be 0-4194302")
    if not 1 <= args.bacnet_port <= 65535:
        raise SoakError("BACnet port must be 1-65535")
    for label in ("duration", "interval", "timeout", "maximum_temperature_c"):
        value = getattr(args, label)
        if not math.isfinite(value):
            raise SoakError(f"{label.replace('_', ' ')} must be finite")
    if args.duration <= 0 or args.interval <= 0 or args.timeout <= 0:
        raise SoakError("duration, interval, and timeout must be greater than zero")
    if args.duration < args.interval:
        raise SoakError("duration must be at least one interval")
    if args.interval < args.timeout * 2:
        raise SoakError("interval must be at least twice the request timeout")
    if args.summary_every <= 0:
        raise SoakError("summary interval must be positive")
    if args.minimum_heap_bytes < 0:
        raise SoakError("minimum heap must not be negative")
    if not args.certificate.is_file():
        raise SoakError(f"pinned certificate not found: {args.certificate}")
    if not args.token_file.is_file():
        raise SoakError(f"token file not found: {args.token_file}")


def build_parser() -> argparse.ArgumentParser:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device-address", required=True)
    parser.add_argument("--device-instance", required=True, type=int)
    parser.add_argument("--bacnet-port", type=int, default=47808)
    parser.add_argument("--duration", type=float, default=86400.0)
    parser.add_argument("--interval", type=float, default=60.0)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--minimum-heap-bytes", type=int, default=1_000_000)
    parser.add_argument("--maximum-temperature-c", type=float, default=85.0)
    parser.add_argument("--summary-every", type=int, default=10)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--certificate",
        type=Path,
        default=root / "main" / "ota_server_cert.pem",
    )
    parser.add_argument(
        "--token-file",
        type=Path,
        default=root / "secrets" / "ota_token.txt",
    )
    return parser


def run(args: argparse.Namespace) -> int:
    validate_args(args)
    token = ota_client._read_token(args.token_file)
    log = JsonlLog(args.output)
    started_monotonic = time.monotonic()
    stats = MonitorStats(args.duration, utc_now())
    baseline: Baseline | None = None
    previous_status: dict[str, Any] | None = None
    interrupted = False
    try:
        for sequence, scheduled in enumerate(sample_schedule(args.duration, args.interval)):
            delay = started_monotonic + scheduled - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            sampled_at = utc_now()
            try:
                status, status_ms = fetch_json(
                    args.device_address, args.certificate, token, "/ota/status", args.timeout
                )
                config, config_ms = fetch_json(
                    args.device_address, args.certificate, token, "/config", args.timeout
                )
                network_config, network_ms = fetch_json(
                    args.device_address,
                    args.certificate,
                    token,
                    "/network/config",
                    args.timeout,
                )
                if baseline is None:
                    baseline = Baseline.from_values(status, config, network_config)
                bacnet, bacnet_ms = probe_bacnet(
                    args.device_address,
                    baseline.device_instance,
                    baseline.bacnet_port,
                    args.timeout,
                )
                alerts = evaluate_sample(
                    baseline,
                    previous_status,
                    status,
                    config,
                    network_config,
                    bacnet,
                    minimum_heap_bytes=args.minimum_heap_bytes,
                    maximum_temperature_c=args.maximum_temperature_c,
                )
                https_ms = status_ms + config_ms + network_ms
                stats.record_success(status, https_ms, bacnet_ms, alerts)
                log.write(
                    {
                        "type": "sample",
                        "sequence": sequence,
                        "sampled_at": sampled_at,
                        "scheduled_seconds": scheduled,
                        "https_latency_ms": round(https_ms, 3),
                        "bacnet_latency_ms": round(bacnet_ms, 3),
                        "alerts": alerts,
                        "status": status,
                        "configuration": config,
                        "network_configuration": network_config,
                        "bacnet_probe": bacnet,
                    }
                )
                previous_status = status
                outcome = "PASS" if not alerts else "ALERT"
                print(
                    f"[{outcome}] sample {sequence}: HTTPS {https_ms:.1f} ms; "
                    f"BACnet {bacnet_ms:.1f} ms; alerts={len(alerts)}",
                    flush=True,
                )
            except (SoakError, KeyError, TypeError, ValueError) as error:
                message = f"{type(error).__name__}:{error}"
                stats.record_failure(message)
                log.write(
                    {
                        "type": "sample",
                        "sequence": sequence,
                        "sampled_at": sampled_at,
                        "scheduled_seconds": scheduled,
                        "error": message,
                    }
                )
                print(f"[FAIL] sample {sequence}: {message}", flush=True)
            if (sequence + 1) % args.summary_every == 0:
                print(
                    f"Progress: {scheduled:.0f}/{args.duration:.0f} seconds; "
                    f"samples={stats.samples}; failures={stats.request_failures}; "
                    f"alerts={stats.samples_with_alerts}",
                    flush=True,
                )
    except KeyboardInterrupt:
        interrupted = True
        print("Interrupted; writing final summary", file=sys.stderr)
    finally:
        elapsed = time.monotonic() - started_monotonic
        summary = stats.summary(
            finished_at=utc_now(), elapsed_seconds=elapsed, interrupted=interrupted
        )
        log.write(summary, sync=True)
        log.close()
        print(json.dumps(summary, indent=2, sort_keys=True), flush=True)
    if interrupted:
        return 130
    return 0 if summary["success"] else 1


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return run(args)
    except (KeyboardInterrupt, SystemExit):
        raise
    except (SoakError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
