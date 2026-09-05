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
