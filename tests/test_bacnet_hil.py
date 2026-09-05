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
        "viewer_token_file": None,
        "certificate": Path("main/ota_server_cert.pem"),
        "mdns_hostname": None,
    }
    values.update(overrides)
    return argparse.Namespace(**values)


class BacnetHilTests(unittest.TestCase):
    def test_expected_object_map_preserves_indexes(self) -> None:
        objects = bacnet_hil_test.expected_object_identifiers(599152)
        self.assertEqual(len(objects), 27)
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
        self.assertEqual(objects[18:], [f"analog-value,{i}" for i in range(1011, 1020)])
        self.assertEqual(len(bacnet_hil_test.INPUT_DIAGNOSTIC_POINTS), 9)
        self.assertEqual(bacnet_hil_test.INPUT_DIAGNOSTIC_POINTS[1019], ("GPIO22 Transition Age", "seconds"))

    def test_network_and_identity_validation(self) -> None:
        item = {"initialized": True, "transition_age_ms": 5000,
                "initial_observation_uptime_ms": 400, "last_transition_uptime_ms": 0,
                "transition_count": 0}
        self.assertTrue(bacnet_hil_test.input_transition_age_valid(item, 5400))
        self.assertFalse(bacnet_hil_test.input_transition_age_valid(item, 9999))
        item.update(transition_count=1, last_transition_uptime_ms=3000, transition_age_ms=2400)
        self.assertTrue(bacnet_hil_test.input_transition_age_valid(item, 5400))
        item["initialized"] = False
        self.assertFalse(bacnet_hil_test.input_transition_age_valid(item, 5400))
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
            "ota_policy": {
                "minimum_image_bytes": 288,
                "maximum_image_bytes": 4194304,
                "upload_deadline_seconds": 300,
                "required_content_type": "application/octet-stream",
                "minimum_secure_version": 0,
            },
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

    def test_input_history_validation(self) -> None:
        history = {
            "capacity": 64, "count": 2, "total_events": 4,
            "overwritten_events": 2, "persistent": False,
            "sample_interval_ms": 10, "captured_uptime_ms": 5000000000,
            "events": [
                {"sequence": 3, "uptime_ms": 4300000000, "gpio": 20,
                 "type": "state-changed", "active": True, "pulse_width_ms": 0,
                 "utc_unix_ms": None, "clock_quality": "unsynchronized"},
                {"sequence": 4, "uptime_ms": 4300000040, "gpio": 21,
                 "type": "rejected-pulse", "active": False, "pulse_width_ms": 20,
                 "utc_unix_ms": 1700000000000, "clock_quality": "synchronized"},
            ],
        }
        self.assertTrue(bacnet_hil_test.input_history_valid({"input_history": history}))
        for field, value in (("count", 3), ("total_events", 1), ("persistent", True),
                             ("captured_uptime_ms", 100), ("count", True)):
            invalid = {**history, field: value}
            self.assertFalse(bacnet_hil_test.input_history_valid({"input_history": invalid}))
        for field, value in (("sequence", 9), ("uptime_ms", -1), ("gpio", 23),
                             ("type", "unknown"), ("active", 1), ("pulse_width_ms", 1)):
            invalid = json.loads(json.dumps(history))
            invalid["events"][0][field] = value
            self.assertFalse(bacnet_hil_test.input_history_valid({"input_history": invalid}))

    def test_clock_diagnostics_and_unknown_event_time(self) -> None:
        clock = {
            "source": "dhcp-option-42", "configured_server": "", "initialized": True,
            "captured_uptime_ms": 10000, "utc_unix_ms": None,
            "clock_quality": "unsynchronized", "sync_count": 0, "rejected_sync_count": 0,
            "last_sync_uptime_ms": None, "last_sync_unix_ms": None, "sync_age_ms": None,
            "stale_after_ms": 7200000, "update_interval_ms": 3600000,
            "last_error": {"code": 0, "name": "ESP_OK"}, "authenticated_time": False,
        }
        status = {"clock": clock, "fault_log": [{"utc_unix_ms": None, "clock_quality": "unsynchronized"}]}
        self.assertTrue(bacnet_hil_test.clock_diagnostics_valid(status))
        clock.update(sync_count=1, clock_quality="synchronized", last_sync_uptime_ms=2000,
                     last_sync_unix_ms=1700000000000, sync_age_ms=8000, utc_unix_ms=1700000008000)
        self.assertTrue(bacnet_hil_test.clock_diagnostics_valid(status))
        clock.update(captured_uptime_ms=7202001, sync_age_ms=7200001,
                     utc_unix_ms=1700007200001, clock_quality="stale")
        self.assertTrue(bacnet_hil_test.clock_diagnostics_valid(status))
        clock["sync_age_ms"] = 1
        self.assertFalse(bacnet_hil_test.clock_diagnostics_valid(status))
        self.assertFalse(bacnet_hil_test.event_clock_valid({"utc_unix_ms": 0, "clock_quality": "unsynchronized"}))
        self.assertFalse(bacnet_hil_test.event_clock_valid({"utc_unix_ms": None, "clock_quality": "synchronized"}))
        self.assertFalse(bacnet_hil_test.event_clock_valid({"clock_quality": "unsynchronized"}))

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

    def test_security_recovery_posture_validation(self) -> None:
        status = {
            "security": {
                "https_management": True,
                "bearer_authentication": True,
                "viewer_admin_separation": True,
                "mutations_require_admin": True,
                "tls_private_key_embedded": True,
                "secure_boot_enabled": False,
                "flash_encryption_enabled": False,
                "application_anti_rollback_enabled": False,
            },
            "recovery": {
                "ota_rollback_enabled": True,
                "task_watchdog_enabled": True,
                "task_watchdog_timeout_seconds": 5,
                "task_watchdog_panics": True,
                "interrupt_watchdog_enabled": True,
                "panic_reboots": True,
                "brownout_detection_enabled": True,
                "core_dump_destination": "disabled",
            },
        }
        self.assertTrue(
            bacnet_hil_test.security_recovery_posture_valid(status)
        )
        status["recovery"]["task_watchdog_panics"] = False
        self.assertFalse(
            bacnet_hil_test.security_recovery_posture_valid(status)
        )

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
