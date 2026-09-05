#!/usr/bin/env python3
"""Host tests for the ESP32-P4 soak monitor."""

from __future__ import annotations

import argparse
import json
import stat
from pathlib import Path
import tempfile
import unittest

from tools import soak_monitor


I_AM = bytes.fromhex(
    "81 0a 00 15 01 00 10 00 c4 02 09 24 71 22 05 c4 91 03 22 03 e7"
)


def healthy_values() -> tuple[dict[str, object], dict[str, object], dict[str, object]]:
    status = {
        "project": "esp32_p4_bacnet_switches",
        "version": "1.9.1",
        "git_revision": "818c5eeb7db8",
        "image_sha256": "d" * 64,
        "partition": "ota_0",
        "state": "valid",
        "system": {
            "boot_count": 32,
            "reset_reason": {"code": 3, "name": "software"},
            "uptime_ms": 100000,
            "free_heap_bytes": 33000000,
            "minimum_free_heap_bytes": 32900000,
            "chip_temperature_c": 35.0,
            "task_watchdog": {
                "bacnet": {"healthy": True},
                "switch_inputs": {"healthy": True},
            },
        },
        "network": {
            "ipv4": "192.168.75.152",
            "mac": "E8:F6:0A:E4:45:8F",
            "link_up": True,
            "full_duplex": True,
            "autonegotiation": True,
            "speed_mbps": 100,
            "link_up_count": 1,
            "link_down_count": 0,
            "reconnect_count": 0,
            "ip_acquisition_count": 1,
            "ip_changed_count": 0,
        },
        "configuration": {
            "active_database_revision": 5,
            "saved_database_revision": 5,
            "restart_required": False,
        },
        "network_configuration": {
            "active_revision": 3,
            "saved_revision": 3,
            "restart_required": False,
            "trial_active": False,
        },
        "bacnet": {
            "rx": 100,
            "responses": 95,
            "who_is": 10,
            "who_has": 2,
            "read_property": 50,
            "read_property_multiple": 4,
            "subscribe_cov": 8,
            "cov_sent": 4,
            "cov_acked": 2,
            "ignored": 1,
            "errors": 3,
            "malformed": 0,
            "rate_limited": 0,
            "cov_timeouts": 0,
            "active_cov_subscriptions": 0,
        },
        "gpio_diagnostics": [
            {"gpio": 20, "fault": False, "signal": {"chattering": False}},
            {"gpio": 21, "fault": False, "signal": {"chattering": False}},
            {"gpio": 22, "fault": False, "signal": {"chattering": False}},
        ],
        "fault_log": [{"sequence": 1, "type": "boot"}],
    }
    config = {
        "device_instance": 599152,
        "vendor_identifier": 999,
        "bacnet_port": 47808,
        "database_revision": 5,
    }
    network_config = {"mode": "dhcp", "revision": 3, "hostname": "esp32-p4-bacnet"}
    return status, config, network_config


class SoakMonitorTests(unittest.TestCase):
    def test_parse_i_am(self) -> None:
        parsed = soak_monitor.parse_i_am(I_AM)
        self.assertEqual(parsed["device_instance"], 599153)
        self.assertEqual(parsed["max_apdu"], 1476)
        self.assertEqual(parsed["segmentation"], 3)
        self.assertEqual(parsed["vendor_identifier"], 999)

    def test_i_am_rejects_bad_length_and_service(self) -> None:
        with self.assertRaisesRegex(soak_monitor.SoakError, "length mismatch"):
            soak_monitor.parse_i_am(I_AM[:-1])
        wrong_service = bytearray(I_AM)
        wrong_service[7] = 1
        with self.assertRaisesRegex(soak_monitor.SoakError, "not an I-Am"):
            soak_monitor.parse_i_am(bytes(wrong_service))

    def test_fingerprint_is_order_independent(self) -> None:
        self.assertEqual(
            soak_monitor.canonical_fingerprint({"a": 1, "b": 2}),
            soak_monitor.canonical_fingerprint({"b": 2, "a": 1}),
        )

    def test_schedule_includes_duration_boundary(self) -> None:
        self.assertEqual(
            list(soak_monitor.sample_schedule(16.0, 5.0)),
            [0.0, 5.0, 10.0, 15.0, 16.0],
        )

    def test_healthy_sample_has_no_alerts(self) -> None:
        status, config, network_config = healthy_values()
        baseline = soak_monitor.Baseline.from_values(status, config, network_config)
        next_status = json.loads(json.dumps(status))
        next_status["system"]["uptime_ms"] = 160000
        next_status["bacnet"]["rx"] = 101
        bacnet = {
            "device_instance": 599152,
            "vendor_identifier": 999,
            "max_apdu": 1476,
            "segmentation": 3,
        }
        alerts = soak_monitor.evaluate_sample(
            baseline,
            status,
            next_status,
            config,
            network_config,
            bacnet,
            minimum_heap_bytes=1_000_000,
            maximum_temperature_c=85.0,
        )
        self.assertEqual(alerts, [])

    def test_gpio_signal_diagnostic_alerts(self) -> None:
        status, config, network_config = healthy_values()
        baseline = soak_monitor.Baseline.from_values(status, config, network_config)
        bacnet = {
            "device_instance": 599152,
            "vendor_identifier": 999,
            "max_apdu": 1476,
            "segmentation": 3,
        }

        chattering = json.loads(json.dumps(status))
        chattering["gpio_diagnostics"][1]["signal"]["chattering"] = True
        alerts = soak_monitor.evaluate_sample(
            baseline,
            status,
            chattering,
            config,
            network_config,
            bacnet,
            minimum_heap_bytes=1_000_000,
            maximum_temperature_c=85.0,
        )
        self.assertIn("gpio-chattering:21", alerts)

        missing = json.loads(json.dumps(status))
        del missing["gpio_diagnostics"][2]["signal"]
        alerts = soak_monitor.evaluate_sample(
            baseline,
            status,
            missing,
            config,
            network_config,
            bacnet,
            minimum_heap_bytes=1_000_000,
            maximum_temperature_c=85.0,
        )
        self.assertIn("gpio-signal-missing:22", alerts)

    def test_reboot_config_network_and_resource_changes_alert(self) -> None:
        status, config, network_config = healthy_values()
        baseline = soak_monitor.Baseline.from_values(status, config, network_config)
        changed = json.loads(json.dumps(status))
        changed["system"].update(
            {"boot_count": 33, "uptime_ms": 10, "free_heap_bytes": 900000}
        )
        changed["network"]["link_down_count"] = 1
        changed["bacnet"]["errors"] = 4
        changed_config = dict(config)
        changed_config["database_revision"] = 6
        bacnet = {
            "device_instance": 599152,
            "vendor_identifier": 999,
            "max_apdu": 1476,
            "segmentation": 3,
        }
        alerts = soak_monitor.evaluate_sample(
            baseline,
            status,
            changed,
            changed_config,
            network_config,
            bacnet,
            minimum_heap_bytes=1_000_000,
            maximum_temperature_c=85.0,
        )
        joined = " ".join(alerts)
        for expected in (
            "boot-count-changed",
            "free-heap-below-floor",
            "configuration-content-changed",
            "uptime-decreased",
            "bacnet-errors-increased",
            "network-link_down_count-increased",
        ):
            self.assertIn(expected, joined)

    def test_jsonl_log_refuses_overwrite(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "soak.jsonl"
            log = soak_monitor.JsonlLog(path)
            log.write({"type": "sample", "sequence": 0}, sync=True)
            log.close()
            self.assertEqual(json.loads(path.read_text())["sequence"], 0)
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
            with self.assertRaisesRegex(soak_monitor.SoakError, "overwrite"):
                soak_monitor.JsonlLog(path)

    def test_argument_safety(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            certificate = root / "certificate.pem"
            token = root / "token.txt"
            certificate.touch()
            token.touch()
            args = argparse.Namespace(
                device_address="192.168.75.152",
                device_instance=599152,
                bacnet_port=47808,
                duration=60.0,
                interval=10.0,
                timeout=4.0,
                maximum_temperature_c=85.0,
                summary_every=1,
                minimum_heap_bytes=1_000_000,
                certificate=certificate,
                token_file=token,
            )
            soak_monitor.validate_args(args)
            args.interval = 5.0
            with self.assertRaisesRegex(soak_monitor.SoakError, "twice"):
                soak_monitor.validate_args(args)
            args.interval = 10.0
            args.duration = float("nan")
            with self.assertRaisesRegex(soak_monitor.SoakError, "finite"):
                soak_monitor.validate_args(args)

    def test_summary_marks_alerts_as_failure(self) -> None:
        stats = soak_monitor.MonitorStats(60.0, "2026-01-01T00:00:00Z")
        status, _, _ = healthy_values()
        stats.record_success(status, 1.0, 2.0, ["network-link_up-unhealthy"])
        summary = stats.summary(
            finished_at="2026-01-01T00:01:00Z",
            elapsed_seconds=60.0,
            interrupted=False,
        )
        self.assertFalse(summary["success"])
        self.assertEqual(summary["samples_with_alerts"], 1)


if __name__ == "__main__":
    unittest.main()
