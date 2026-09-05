#!/usr/bin/env python3
"""Tests for permission-restricted private firmware packaging."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


PROJECT_ROOT = Path(__file__).resolve().parents[1]
TOOLS = PROJECT_ROOT / "tools"
PACKAGER = TOOLS / "package_release.py"


def _load_packager():
    sys.path.insert(0, str(TOOLS))
    specification = importlib.util.spec_from_file_location(
        "package_release", PACKAGER
    )
    if specification is None or specification.loader is None:
        raise RuntimeError("could not load package_release.py")
    module = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    return module


def _firmware(revision: bytes = b"abcdef123456") -> bytes:
    header = bytearray(24)
    header[0] = 0xE9
    header[1] = 1
    header[2] = 2
    header[3] = 0x5F
    struct.pack_into("<H", header, 12, 18)
    header[23] = 1

    segment = bytearray(320)
    struct.pack_into("<I", segment, 0, 0xABCD5432)
    segment[16:22] = b"9.8.7\0"
    project = b"esp32_p4_bacnet_switches"
    segment[48 : 48 + len(project)] = project
    segment[200 : 200 + len(revision)] = revision
    image = header + struct.pack("<II", 0x48080020, len(segment)) + segment
    checksum = 0xEF
    for byte in segment:
        checksum ^= byte
    while len(image) % 16 != 15:
        image.append(0)
    image.append(checksum)
    image.extend(hashlib.sha256(image).digest())
    return bytes(image)


def _write(path: Path, value: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(value)


class PackageReleaseTests(unittest.TestCase):
    def setUp(self) -> None:
        self.packager = _load_packager()

    def _fixture(self, root: Path) -> tuple[Path, Path]:
        build = root / "build"
        app = build / "esp32_p4_bacnet_switches.bin"
        _write(app, _firmware())
        _write(build / "bootloader" / "bootloader.bin", b"bootloader")
        _write(build / "partition_table" / "partition-table.bin", b"table")
        _write(build / "ota_data_initial.bin", b"ota-data")
        (build / "project_git_revision.txt").write_text(
            "abcdef123456\n", encoding="ascii"
        )
        (build / "config").mkdir(parents=True)
        (build / "config" / "sdkconfig.json").write_text(
            json.dumps(
                {
                    "OTA_HTTPS_ENABLED": True,
                    "BOOTLOADER_APP_ROLLBACK_ENABLE": True,
                    "ESP_TASK_WDT_EN": True,
                    "ESP_TASK_WDT_INIT": True,
                    "ESP_TASK_WDT_PANIC": True,
                    "ESP_COREDUMP_ENABLE_TO_NONE": True,
                    "SECURE_BOOT": False,
                    "SECURE_FLASH_ENC_ENABLED": False,
                }
            ),
            encoding="utf-8",
        )

        idf = root / "esp-idf"
        license_paths = (
            "LICENSE",
            "components/freertos/FreeRTOS-Kernel/LICENSE.md",
            "components/http_parser/LICENSE.txt",
            "components/json/cJSON/LICENSE",
            "components/lwip/lwip/COPYING",
            "components/mbedtls/mbedtls/LICENSE",
        )
        for relative in license_paths:
            _write(idf / relative, f"license: {relative}\n".encode())

        flasher = {
            "flash_settings": {
                "flash_mode": "dio",
                "flash_size": "32MB",
                "flash_freq": "80m",
            },
            "extra_esptool_args": {"chip": "esp32p4"},
            "flash_files": {
                "0x2000": "bootloader/bootloader.bin",
                "0x8000": "partition_table/partition-table.bin",
                "0xf000": "ota_data_initial.bin",
                "0x20000": "esp32_p4_bacnet_switches.bin",
            },
            "app": {
                "offset": "0x20000",
                "file": "esp32_p4_bacnet_switches.bin",
            },
        }
        (build / "flasher_args.json").write_text(
            json.dumps(flasher), encoding="utf-8"
        )
        (build / "project_description.json").write_text(
            json.dumps(
                {
                    "target": "esp32p4",
                    "project_name": "esp32_p4_bacnet_switches",
                    "project_version": "9.8.7",
                    "idf_path": str(idf),
                    "git_revision": "v5.5.4",
                }
            ),
            encoding="utf-8",
        )
        return build, root / "private-output"

    @staticmethod
    def _fake_esptool(command, check, stdout, stderr):
        if not check or "merge_bin" not in command:
            raise AssertionError(command)
        if stdout is not subprocess.PIPE or stderr is not subprocess.STDOUT:
            raise AssertionError("esptool output must not pollute package stdout")
        output = Path(command[command.index("-o") + 1])
        output.write_bytes(b"merged recovery image")
        return subprocess.CompletedProcess(command, 0)

    def test_package_is_private_complete_hashed_and_never_overwritten(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            build, output_root = self._fixture(Path(temporary))
            with mock.patch.object(
                self.packager.subprocess,
                "run",
                side_effect=self._fake_esptool,
            ):
                output = self.packager.package_release(build, output_root)
                with self.assertRaisesRegex(ValueError, "refusing to overwrite"):
                    self.packager.package_release(build, output_root)

            self.assertEqual(output, output_root / "v9.8.7")
            expected = {
                "firmware-ota.bin",
                "initial-flash.bin",
                "flash-args.txt",
                "manifest.json",
                "SHA256SUMS",
                "management/ota_client.py",
                "management/ota_server_cert.pem",
                "docs/COMMISSIONING.md",
                "docs/SECURITY.md",
                "THIRD_PARTY_NOTICES.md",
                "licenses/ESP-IDF-Apache-2.0.txt",
                "licenses/esp-protocols-mDNS-Apache-2.0.txt",
            }
            paths = {
                path.relative_to(output).as_posix()
                for path in output.rglob("*")
                if path.is_file()
            }
            self.assertTrue(expected.issubset(paths))
            self.assertFalse(any("token" in path.lower() for path in paths))
            self.assertFalse(any("private_key" in path.lower() for path in paths))

            manifest = json.loads((output / "manifest.json").read_text())
            self.assertTrue(manifest["sensitive"])
            self.assertEqual(manifest["chip"], "esp32p4")
            self.assertEqual(manifest["version"], "9.8.7")
            self.assertEqual(manifest["source_revision"], "abcdef123456")
            self.assertTrue(manifest["security"]["ota_rollback"])
            self.assertFalse(manifest["security"]["secure_boot"])

            for line in (output / "SHA256SUMS").read_text().splitlines():
                expected_hash, relative = line.split("  ", 1)
                self.assertEqual(
                    self.packager.sha256_file(output / relative), expected_hash
                )
            if os.name == "posix":
                self.assertEqual(
                    stat.S_IMODE(output.stat().st_mode),
                    self.packager.PRIVATE_DIRECTORY_MODE,
                )
                for path in output.rglob("*"):
                    expected_mode = 0o700 if path.is_dir() else 0o600
                    self.assertEqual(stat.S_IMODE(path.stat().st_mode), expected_mode)

    def test_rejects_build_path_escape_dirty_revision_and_symlink_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build, output_root = self._fixture(root)
            outside = root / "outside.bin"
            outside.write_bytes(b"outside")
            flasher_path = build / "flasher_args.json"
            flasher = json.loads(flasher_path.read_text())
            flasher["flash_files"]["0x2000"] = "../outside.bin"
            flasher_path.write_text(json.dumps(flasher), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "escapes the build directory"):
                self.packager.package_release(build, output_root)

            build, output_root = self._fixture(root / "dirty")
            (build / "project_git_revision.txt").write_text(
                "abcdef123456-dirty\n", encoding="ascii"
            )
            with self.assertRaisesRegex(ValueError, "dirty worktree"):
                self.packager.package_release(build, output_root)

            if hasattr(os, "symlink"):
                build, _ = self._fixture(root / "linked")
                real_output = root / "real-output"
                real_output.mkdir(mode=0o700)
                linked_output = root / "linked-output"
                linked_output.symlink_to(real_output, target_is_directory=True)
                with self.assertRaisesRegex(ValueError, "symbolic link"):
                    self.packager.package_release(build, linked_output)


if __name__ == "__main__":
    unittest.main()
