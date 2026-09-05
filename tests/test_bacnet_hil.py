#!/usr/bin/env python3
"""Host tests for the BACnet hardware-in-the-loop runner."""

from __future__ import annotations

import argparse
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import tempfile
import unittest

from tools import bacnet_hil_test


def args(**overrides: object) -> argparse.Namespace:
    values = {
        "local_address": "192.168.75.191/24",
        "device_address": "192.168.75.152",
        "device_instance": 599152,
        "client_instance": 4194001,
        "bacnet_port": 47808,
        "timeout": 4.0,
        "expected_source": None,
        "expected_image_sha256": None,
        "token_file": None,
        "certificate": Path("main/ota_server_cert.pem"),
        "mdns_hostname": None,
    }
    values.update(overrides)
    return argparse.Namespace(**values)


class BacnetHilTests(unittest.TestCase):
    def test_expected_object_map_preserves_indexes(self) -> None:
        objects = bacnet_hil_test.expected_object_identifiers(599152)
        self.assertEqual(len(objects), 18)
        self.assertEqual(
            objects[:4],
            [
                "device,599152",
                "binary-input,20",
                "binary-input,21",
                "binary-input,22",
            ],
        )
        self.assertEqual(
            objects[14:17],
            ["network-port,1", "binary-input,1001", "binary-input,1002"],
        )
        self.assertEqual(objects[17], "analog-value,1010")

    def test_network_and_identity_validation(self) -> None:
        bacnet_hil_test.validate_args(args())
        with self.assertRaisesRegex(bacnet_hil_test.HilError, "subnet"):
            bacnet_hil_test.validate_args(args(device_address="192.168.76.152"))
        with self.assertRaisesRegex(bacnet_hil_test.HilError, "must differ"):
            bacnet_hil_test.validate_args(args(client_instance=599152))
        with self.assertRaisesRegex(bacnet_hil_test.HilError, "greater than zero"):
            bacnet_hil_test.validate_args(args(timeout=0))
        with self.assertRaisesRegex(bacnet_hil_test.HilError, "RFC 1123"):
            bacnet_hil_test.validate_args(args(mdns_hostname="bad hostname"))

    def test_expected_identity_format_validation(self) -> None:
        bacnet_hil_test.validate_args(
            args(
                expected_source="cec1ae4bfbd5",
                expected_image_sha256="8" * 64,
            )
        )
        with self.assertRaisesRegex(bacnet_hil_test.HilError, "expected-source"):
            bacnet_hil_test.validate_args(args(expected_source="not-a-revision"))
        with self.assertRaisesRegex(bacnet_hil_test.HilError, "64 hexadecimal"):
            bacnet_hil_test.validate_args(args(expected_image_sha256="8" * 63))

    def test_error_signature_normalizes_names(self) -> None:
        error = RuntimeError("failure")
        error.errorClass = "object"
        error.errorCode = "unknown_object"
        self.assertEqual(
            bacnet_hil_test.error_signature(error),
            ("object", "unknown-object"),
        )

    def test_revision_match_accepts_full_and_abbreviated_forms(self) -> None:
        full = "ad7a24fdb9dc81e07c5f3a5e35afdb7ba6b8a887"
        self.assertTrue(bacnet_hil_test.revision_matches(full, "1.11.0 (ad7a24fdb9dc)"))
        self.assertTrue(bacnet_hil_test.revision_matches(full[:12], full))
        self.assertTrue(bacnet_hil_test.revision_matches(None, "unknown"))
        self.assertFalse(bacnet_hil_test.revision_matches(full, "1.11.0 (b583bcdbe73e)"))
        self.assertFalse(bacnet_hil_test.revision_matches(full, "ad7a24fdb9dc-dirty"))

    def test_hardware_profile_validation_is_exact(self) -> None:
        profile = json.loads(json.dumps(bacnet_hil_test.EXPECTED_HARDWARE_PROFILE))
        self.assertTrue(bacnet_hil_test.hardware_profile_valid(profile))
        profile["inputs"][0]["header_position"] = 36
        self.assertFalse(bacnet_hil_test.hardware_profile_valid(profile))

    def test_firmware_diagnostics_validation(self) -> None:
        image_sha = "a" * 64
        status = {
            "partition": "ota_0",
            "state": "valid",
            "image_sha256": image_sha,
            "firmware": {
                "build_date": "Sep  5 2026",
                "build_time": "04:00:00",
                "secure_version": 0,
                "elf_sha256": "b" * 64,
                "rollback_enabled": True,
                "running_partition": {
                    "label": "ota_0",
                    "address": 131072,
                    "size_bytes": 4194304,
                    "state": "valid",
                    "image_sha256": image_sha,
                },
                "boot_partition": {
                    "label": "ota_0",
                    "address": 131072,
                    "size_bytes": 4194304,
                    "matches_running": True,
                },
                "next_update_partition": {
                    "available": True,
                    "label": "ota_1",
                    "address": 4325376,
                    "size_bytes": 4194304,
                },
            },
        }
        self.assertTrue(bacnet_hil_test.firmware_diagnostics_valid(status))
        status["firmware"]["next_update_partition"]["label"] = "ota_0"
        self.assertFalse(bacnet_hil_test.firmware_diagnostics_valid(status))

    def test_runtime_diagnostics_validation(self) -> None:
        status = {
            "system": {
                "chip_temperature_c": 36.5,
                "uptime_ms": 10000,
                "temperature": {
                    "valid": True,
                    "current_c": 36.5,
                    "minimum_c": 35.0,
                    "maximum_c": 37.0,
                    "sample_count_since_boot": 9,
                    "error_count_since_boot": 0,
                    "sample_interval_ms": 1000,
                    "last_sample_uptime_ms": 9500,
                    "sample_age_ms": 500,
                    "last_result": {"code": 0, "name": "ESP_OK"},
                },
            },
            "fault_log": [
                {"sequence": 8, "type": "boot"},
                {"sequence": 9, "type": "ota-validated"},
            ],
            "fault_log_health": {
                "capacity": 16,
                "count": 2,
                "total_event_count": 9,
                "overwritten_event_count": 7,
                "persistence_ready": True,
                "write_failure_count_since_boot": 0,
                "last_write_error": {"code": 0, "name": "ESP_OK"},
                "oldest_sequence": 8,
                "newest_sequence": 9,
            },
        }
        self.assertTrue(bacnet_hil_test.runtime_diagnostics_valid(status))
        status["system"]["temperature"]["maximum_c"] = 30.0
        self.assertFalse(bacnet_hil_test.runtime_diagnostics_valid(status))

    def test_report_is_machine_readable(self) -> None:
        report = bacnet_hil_test.TestReport(
            started_at="2026-01-01T00:00:00Z",
            target={"device_address": "192.0.2.1", "device_instance": 1},
            options={"authenticated_https": False},
        )
        with redirect_stdout(io.StringIO()):
            report.add("discovery", "pass", "one device")
            report.add("HTTPS", "skip", "no token")
        report.finish()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "report.json"
            bacnet_hil_test.write_report(path, report)
            saved = json.loads(path.read_text(encoding="utf-8"))
        self.assertTrue(saved["summary"]["success"])
        self.assertEqual(saved["summary"]["passed"], 1)
        self.assertEqual(saved["summary"]["skipped"], 1)


if __name__ == "__main__":
    unittest.main()
