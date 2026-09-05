from contextlib import redirect_stderr
import copy
import io
import json
import unittest
from unittest import mock

from tools import verify_live_readonly as verifier


def cli_args():
    return ["--host", "192.0.2.1", "--expected-version", "1.28.0",
            "--expected-source", "a" * 12, "--expected-image-sha256", "b" * 64,
            "--samples", "2"]


def status_fixture():
    header = {"header": "P1", "position_count": 40, "gpio_count": 27,
              "readable_count": 25, "initialized": True, "initialization_ok": True,
              "sample_mode": "sequential-on-request", "captured_uptime_ms": 100,
              "completed_uptime_ms": 101, "pins": []}
    for position, gpio in enumerate(verifier.bacnet_hil_test.P1_GPIO_MAP, 1):
        readable = gpio is not None and gpio not in (24, 25)
        kind = ("gpio" if gpio is not None else "control" if position in (30, 37)
                else "supply" if position in (36, 39, 40) else "ground")
        header["pins"].append({
            "position": position, "gpio": gpio, "label": "pin", "usage": "test",
            "kind": kind, "status": "readable" if readable else "non-gpio" if gpio is None else "reserved-usb",
            "raw_level": False if readable else None,
            "input_enabled_by_diagnostics": readable,
            "initialization_preserved_config": True if readable else None,
            "pad": {"input_enabled": True, "output_enabled": False,
                    "output_enable_controlled_by_peripheral": False, "pull_up": False,
                    "pull_down": False, "function_select": 1} if readable else None,
        })
    return {"project": verifier.ota_client.DEFAULT_PROJECT, "version": "1.28.0",
            "git_revision": "a" * 12, "state": "valid", "access_role": "anonymous",
            "image_sha256": "b" * 64, "partition": "ota_0", "system": {"boot_count": 1},
            "header_diagnostics": header}


class LiveReadonlyTests(unittest.TestCase):
    def setUp(self):
        self.args = verifier.build_parser().parse_args(cli_args())
        self.status = status_fixture()
        self.calls = []
        self.counts = {}
        self.mode = "ok"

    def payload(self, args, route):
        self.calls.append(route)
        self.counts[route] = self.counts.get(route, 0) + 1
        if route in verifier.ASSETS:
            return b"wrong" if self.mode == "asset" else (verifier.ROOT / "main" / verifier.ASSETS[route]).read_bytes()
        status = copy.deepcopy(self.status)
        if route == "/ota/status":
            if self.counts[route] > 1:
                if self.mode == "boot": status["system"]["boot_count"] += 1
                if self.mode == "pad": status["header_diagnostics"]["pins"][0]["pad"]["pull_up"] = True
                if self.mode == "level": status["header_diagnostics"]["pins"][0]["raw_level"] = True
            value = status
        elif route == "/diagnostics/report":
            value = {"schema": 1, "report_type": "esp32-p4-diagnostics", "status": status,
                     **{name: {} for name in ("active_configuration", "saved_configuration",
                        "active_network_configuration", "saved_network_configuration", "confirmed_network_configuration")}}
            if self.mode == "credential": value["ota_token"] = "synthetic-test-value"
        else:
            value = {"revision": 2 if self.mode == "config" and self.counts[route] > 1 else 1}
        return json.dumps(value).encode()

    def run_check(self):
        with mock.patch.object(verifier, "get", side_effect=self.payload), mock.patch.object(verifier.time, "sleep"):
            return verifier.verify(self.args)

    def test_complete_pass_and_only_read_routes(self):
        result = self.run_check()
        self.assertTrue(result["success"])
        self.assertFalse(result["known_token_values_checked"])
        self.assertEqual(result["positions"], 40)
        self.assertEqual(result["readable_gpios"], 25)
        self.assertEqual(result["observed_source"], "a" * 12)
        self.assertEqual(result["partition"], "ota_0")
        self.assertEqual(result["boot_count"], 1)
        self.assertEqual(set(result), {"success", "samples", "version", "expected_source",
                         "image_sha256", "observed_source", "partition", "boot_count",
                         "positions", "readable_gpios", "configuration_unchanged",
                         "report_structure_checked", "served_assets_match",
                         "known_token_values_checked", "high_positions"})
        self.assertEqual(set(self.calls), {"/config", "/network/config", "/ota/status", "/diagnostics/report", *verifier.ASSETS})

    def test_physical_level_changes_are_allowed(self):
        self.mode = "level"
        result = self.run_check()
        self.assertEqual(result["high_positions"], [{"position": 1, "gpio": 54}])

    def test_identity_and_mapping_fail_closed(self):
        for field, value in (("version", "other"), ("git_revision", None), ("git_revision", "a" * 12 + "-dirty"),
                             ("image_sha256", "c" * 64), ("state", "pending"),
                             ("partition", None), ("access_role", "admin")):
            with self.subTest(field=field), self.assertRaises(ValueError):
                verifier.validate_status(self.args, {**self.status, field: value})
        self.status["header_diagnostics"]["pins"][18]["raw_level"] = False
        with self.assertRaises(ValueError): self.run_check()
        for key in ("gpio", "pad", "initialization_preserved_config"):
            self.status = status_fixture()
            del self.status["header_diagnostics"]["pins"][2][key]
            with self.subTest(missing=key), self.assertRaises(ValueError): self.run_check()

    def test_state_asset_and_credential_errors_fail(self):
        for mode in ("boot", "pad", "config", "asset", "credential"):
            self.mode, self.counts = mode, {}
            with self.subTest(mode=mode), self.assertRaises(ValueError): self.run_check()

    def test_transport_is_get_only_and_always_closed(self):
        connection = mock.Mock()
        response = connection.getresponse.return_value
        response.status, response.read.return_value = 200, b"{}"
        with mock.patch.object(verifier.ota_client, "_connection", return_value=connection):
            self.assertEqual(verifier.get(self.args, "/ota/status"), b"{}")
            connection.request.assert_called_once_with("GET", "/ota/status", headers={"Accept": "*/*"})
            connection.close.assert_called_once()
            connection.reset_mock()
            response.status = 302
            with self.assertRaises(ValueError): verifier.get(self.args, "/ota/status")
            connection.close.assert_called_once()

    def test_bad_arguments_never_contact_device(self):
        for extra in (("--samples", "1"), ("--timeout", "nan"), ("--interval", "inf"),
                      ("--port", "0"), ("--expected-source", "dirty")):
            with mock.patch.object(verifier, "get") as get, redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit): verifier.main(cli_args() + list(extra))
                get.assert_not_called()
