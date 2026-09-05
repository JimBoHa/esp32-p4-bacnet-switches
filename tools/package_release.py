#!/usr/bin/env python3
"""Create a permission-restricted ESP32-P4 recovery and OTA package."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import stat
import subprocess
import sys
from typing import Any

from ota_client import DEFAULT_PROJECT, _inspect_firmware


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT_ROOT = ROOT / "release" / "private"
EXPECTED_TARGET = "esp32p4"
EXPECTED_FLASH_SIZE = "32MB"
OTA_PARTITION_BYTES = 0x400000
PRIVATE_DIRECTORY_MODE = 0o700
PRIVATE_FILE_MODE = 0o600
VERSION_PATTERN = re.compile(r"[0-9A-Za-z][0-9A-Za-z._+-]{0,30}\Z")
REVISION_PATTERN = re.compile(r"[0-9a-f]{7,40}\Z")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _absolute(path: Path) -> Path:
    return Path(os.path.abspath(os.fspath(path)))


def _is_within(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def _refuse_symlink_components(path: Path, description: str) -> None:
    absolute = _absolute(path)
    if absolute.is_symlink():
        raise ValueError(f"{description} is a symbolic link: {absolute}")


def _read_json_object(path: Path, description: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read {description} {path}: {error}") from error
    if not isinstance(value, dict):
        raise ValueError(f"{description} must contain a JSON object: {path}")
    return value


def _resolve_build_input(build_dir: Path, value: object, description: str) -> Path:
    if not isinstance(value, str) or not value or Path(value).is_absolute():
        raise ValueError(f"{description} must be a relative build path")
    unresolved = build_dir / value
    _refuse_symlink_components(unresolved, description)
    resolved = unresolved.resolve()
    if not _is_within(resolved, build_dir):
        raise ValueError(f"{description} escapes the build directory: {value}")
    if not resolved.is_file():
        raise ValueError(f"{description} is missing: {resolved}")
    return resolved


def _secure_directory(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True, mode=PRIVATE_DIRECTORY_MODE)
    path.chmod(PRIVATE_DIRECTORY_MODE)


def _secure_write(path: Path, data: bytes) -> None:
    _secure_directory(path.parent)
    descriptor = os.open(
        path,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL,
        PRIVATE_FILE_MODE,
    )
    try:
        output = os.fdopen(descriptor, "wb")
    except BaseException:
        os.close(descriptor)
        raise
    with output:
        output.write(data)
        output.flush()
        os.fsync(output.fileno())
    path.chmod(PRIVATE_FILE_MODE)


def _secure_write_text(path: Path, text: str, encoding: str = "utf-8") -> None:
    _secure_write(path, text.encode(encoding))


def _secure_copy(source: Path, destination: Path) -> None:
    _secure_write(destination, source.read_bytes())


def _validate_output_root(output_root: Path) -> Path:
    output_root = _absolute(output_root)
    _refuse_symlink_components(output_root, "release output path")
    project_root = ROOT.resolve()
    allowed_project_root = (ROOT / "release" / "private").resolve()
    resolved_output = output_root.resolve()
    if _is_within(resolved_output, project_root) and not _is_within(
        resolved_output, allowed_project_root
    ):
        raise ValueError(
            "release output inside the repository must be below ignored "
            f"{allowed_project_root}"
        )
    if output_root.exists():
        if not output_root.is_dir():
            raise ValueError(f"release output root is not a directory: {output_root}")
        if os.name == "posix":
            mode = stat.S_IMODE(output_root.stat().st_mode)
            if mode != PRIVATE_DIRECTORY_MODE:
                raise ValueError(
                    f"release output root must have mode 0700: "
                    f"{output_root} ({mode:04o})"
                )
    else:
        _secure_directory(output_root)
    return output_root


def _validate_build_policy(build_dir: Path) -> dict[str, Any]:
    config = _read_json_object(
        build_dir / "config" / "sdkconfig.json", "generated sdkconfig"
    )
    required_true = (
        "OTA_HTTPS_ENABLED",
        "BOOTLOADER_APP_ROLLBACK_ENABLE",
        "ESP_TASK_WDT_EN",
        "ESP_TASK_WDT_INIT",
        "ESP_TASK_WDT_PANIC",
        "ESP_COREDUMP_ENABLE_TO_NONE",
    )
    missing = [name for name in required_true if config.get(name) is not True]
    if missing:
        raise ValueError(
            "release build is missing required recovery controls: "
            + ", ".join(missing)
        )
    return config


def _build_revision(build_dir: Path, firmware: bytes) -> str:
    revision_path = build_dir / "project_git_revision.txt"
    try:
        revision = revision_path.read_text(encoding="ascii").strip()
    except OSError as error:
        raise ValueError(
            f"cannot read {revision_path}; run idf.py reconfigure build: {error}"
        ) from error
    if revision.endswith("-dirty"):
        raise ValueError("refusing to package a build made from a dirty worktree")
    if not REVISION_PATTERN.fullmatch(revision):
        raise ValueError(f"invalid build source revision: {revision!r}")
    if revision.encode("ascii") not in firmware:
        raise ValueError("build source revision is not embedded in the application image")
    return revision


def _validated_flash_inputs(
    build_dir: Path, flasher: dict[str, Any]
) -> list[tuple[str, Path, str]]:
    flash_files = flasher.get("flash_files")
    if not isinstance(flash_files, dict) or not flash_files:
        raise ValueError("flasher_args.json has no flash_files object")
    if len(flash_files) > 16:
        raise ValueError("flasher_args.json contains too many flash inputs")

    inputs: list[tuple[str, Path, str]] = []
    destinations: set[str] = set()
    for offset_value, relative_value in flash_files.items():
        if not isinstance(offset_value, str):
            raise ValueError("flash offset must be a string")
        try:
            offset = int(offset_value, 0)
        except ValueError as error:
            raise ValueError(f"invalid flash offset: {offset_value!r}") from error
        if offset < 0 or offset >= 32 * 1024 * 1024:
            raise ValueError(f"flash offset is outside 32 MB flash: {offset_value}")
        source = _resolve_build_input(
            build_dir, relative_value, f"flash input at {offset_value}"
        )
        relative = source.relative_to(build_dir).as_posix()
        destination = f"flash/{relative}"
        if destination in destinations:
            raise ValueError(f"duplicate packaged flash path: {destination}")
        destinations.add(destination)
        inputs.append((f"0x{offset:x}", source, destination))
    inputs.sort(key=lambda item: int(item[0], 0))
    return inputs


def _copy_notices(staging: Path, idf_path: Path) -> None:
    project_files = {
        ROOT / "THIRD_PARTY_NOTICES.md": staging / "THIRD_PARTY_NOTICES.md",
        ROOT / "SECURITY.md": staging / "docs" / "SECURITY.md",
        ROOT / "docs" / "COMMISSIONING.md": staging / "docs" / "COMMISSIONING.md",
    }
    for source, destination in project_files.items():
        if not source.is_file():
            raise ValueError(f"required release document is missing: {source}")
        _secure_copy(source, destination)

    licenses = {
        idf_path / "LICENSE": staging / "licenses" / "ESP-IDF-Apache-2.0.txt",
        idf_path / "components" / "freertos" / "FreeRTOS-Kernel" / "LICENSE.md":
            staging / "licenses" / "FreeRTOS-LICENSE.md",
        idf_path / "components" / "http_parser" / "LICENSE.txt":
            staging / "licenses" / "http-parser-LICENSE.txt",
        idf_path / "components" / "json" / "cJSON" / "LICENSE":
            staging / "licenses" / "cJSON-LICENSE.txt",
        idf_path / "components" / "lwip" / "lwip" / "COPYING":
            staging / "licenses" / "lwIP-COPYING.txt",
        idf_path / "components" / "mbedtls" / "mbedtls" / "LICENSE":
            staging / "licenses" / "mbedTLS-LICENSE.txt",
    }
    for source, destination in licenses.items():
        if not source.is_file():
            raise ValueError(f"required third-party license is missing: {source}")
        _secure_copy(source, destination)
    # The mDNS component and ESP-IDF ship the identical Apache-2.0 text. Use
    # the SDK copy so host packaging tests need no downloaded component tree.
    _secure_copy(
        idf_path / "LICENSE",
        staging / "licenses" / "esp-protocols-mDNS-Apache-2.0.txt",
    )


def package_release(build_directory: Path, output_directory: Path) -> Path:
    build_dir = _absolute(build_directory).resolve()
    if not build_dir.is_dir():
        raise ValueError(f"build directory does not exist: {build_dir}")
    flasher = _read_json_object(
        build_dir / "flasher_args.json", "ESP-IDF flasher arguments"
    )
    project = _read_json_object(
        build_dir / "project_description.json", "ESP-IDF project description"
    )
    if project.get("target") != EXPECTED_TARGET:
        raise ValueError(f"release build target is not {EXPECTED_TARGET}")
    if project.get("project_name") != DEFAULT_PROJECT:
        raise ValueError(f"release build project is not {DEFAULT_PROJECT}")
    extra = flasher.get("extra_esptool_args")
    if not isinstance(extra, dict) or extra.get("chip") != EXPECTED_TARGET:
        raise ValueError(f"flasher target is not {EXPECTED_TARGET}")
    settings = flasher.get("flash_settings")
    if not isinstance(settings, dict) or settings.get("flash_size") != EXPECTED_FLASH_SIZE:
        raise ValueError(f"release build is not configured for {EXPECTED_FLASH_SIZE} flash")

    app_entry = flasher.get("app")
    if not isinstance(app_entry, dict):
        raise ValueError("flasher_args.json has no app entry")
    application = _resolve_build_input(
        build_dir, app_entry.get("file"), "application image"
    )
    metadata = _inspect_firmware(application)
    if metadata.project != DEFAULT_PROJECT:
        raise ValueError(f"application project is not {DEFAULT_PROJECT}")
    if project.get("project_version") != metadata.version:
        raise ValueError("project description and application versions differ")
    if not VERSION_PATTERN.fullmatch(metadata.version):
        raise ValueError(f"unsafe application version: {metadata.version!r}")
    if metadata.size > OTA_PARTITION_BYTES:
        raise ValueError("application exceeds the 4 MiB OTA partition")
    source_revision = _build_revision(build_dir, metadata.image)
    config = _validate_build_policy(build_dir)
    flash_inputs = _validated_flash_inputs(build_dir, flasher)
    if not any(source == application for _offset, source, _destination in flash_inputs):
        raise ValueError("application image is absent from the flash input list")

    idf_path_value = project.get("idf_path")
    if not isinstance(idf_path_value, str) or not idf_path_value:
        raise ValueError("project description has no ESP-IDF path")
    idf_path = Path(idf_path_value).resolve()
    if not idf_path.is_dir():
        raise ValueError(f"ESP-IDF path does not exist: {idf_path}")

    output_root = _validate_output_root(output_directory)
    final = output_root / f"v{metadata.version}"
    _refuse_symlink_components(final, "release version path")
    if final.exists() or final.is_symlink():
        raise ValueError(f"refusing to overwrite existing release directory: {final}")
    staging = output_root / (
        f".v{metadata.version}.staging-{os.getpid()}-{secrets.token_hex(4)}"
    )
    staging.mkdir(mode=PRIVATE_DIRECTORY_MODE)

    try:
        ota_output = staging / "firmware-ota.bin"
        _secure_copy(application, ota_output)
        packaged_inputs: list[tuple[str, Path, str]] = []
        for offset, source, destination_name in flash_inputs:
            if source == application:
                destination_name = ota_output.name
            else:
                _secure_copy(source, staging / destination_name)
            packaged_inputs.append((offset, source, destination_name))

        merged_output = staging / "initial-flash.bin"
        merge_command = [
            sys.executable,
            "-m",
            "esptool",
            "--chip",
            EXPECTED_TARGET,
            "merge_bin",
            "-o",
            str(merged_output),
            "--flash_mode",
            str(settings.get("flash_mode", "dio")),
            "--flash_size",
            EXPECTED_FLASH_SIZE,
            "--flash_freq",
            str(settings.get("flash_freq", "80m")),
        ]
        for offset, source, _destination in packaged_inputs:
            merge_command.extend((offset, str(source)))
        try:
            subprocess.run(
                merge_command,
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
        except (OSError, subprocess.CalledProcessError) as error:
            raise RuntimeError(
                "esptool merge failed; run inside the ESP-IDF environment"
            ) from error
        if not merged_output.is_file():
            raise RuntimeError("esptool did not create the merged recovery image")
        merged_output.chmod(PRIVATE_FILE_MODE)

        _secure_write_text(
            staging / "flash-args.txt",
            " ".join(
                item
                for offset, _source, destination in packaged_inputs
                for item in (offset, destination)
            )
            + "\n",
            encoding="ascii",
        )
        _secure_copy(
            ROOT / "main" / "ota_server_cert.pem",
            staging / "management" / "ota_server_cert.pem",
        )
        _secure_copy(
            ROOT / "tools" / "ota_client.py",
            staging / "management" / "ota_client.py",
        )
        _copy_notices(staging, idf_path)

        artifacts: dict[str, dict[str, object]] = {}
        for path in sorted(staging.rglob("*")):
            if path.is_file():
                relative = path.relative_to(staging).as_posix()
                artifacts[relative] = {
                    "bytes": path.stat().st_size,
                    "sha256": sha256_file(path),
                }
        manifest = {
            "schema": 1,
            "sensitive": True,
            "handling": (
                "Contains device credentials inside firmware; keep in an approved "
                "secret store and never attach to a public GitHub release."
            ),
            "target_board": "Waveshare ESP32-P4-POE-ETH",
            "chip": EXPECTED_TARGET,
            "flash_size": EXPECTED_FLASH_SIZE,
            "project": metadata.project,
            "version": metadata.version,
            "source_revision": source_revision,
            "idf_version": project.get("git_revision", "unknown"),
            "ota_partition_bytes": OTA_PARTITION_BYTES,
            "application": {
                "bytes": metadata.size,
                "file_sha256": metadata.file_sha256,
                "image_sha256": metadata.image_sha256,
            },
            "security": {
                "https_ota": bool(config.get("OTA_HTTPS_ENABLED")),
                "ota_rollback": bool(config.get("BOOTLOADER_APP_ROLLBACK_ENABLE")),
                "task_watchdog_panic": bool(config.get("ESP_TASK_WDT_PANIC")),
                "core_dump_disabled": bool(config.get("ESP_COREDUMP_ENABLE_TO_NONE")),
                "secure_boot": bool(config.get("SECURE_BOOT")),
                "flash_encryption": bool(config.get("SECURE_FLASH_ENC_ENABLED")),
            },
            "artifacts": artifacts,
        }
        _secure_write_text(
            staging / "manifest.json",
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        )
        checksummed = [
            path
            for path in sorted(staging.rglob("*"))
            if path.is_file() and path.name != "SHA256SUMS"
        ]
        _secure_write_text(
            staging / "SHA256SUMS",
            "".join(
                f"{sha256_file(path)}  {path.relative_to(staging).as_posix()}\n"
                for path in checksummed
            ),
            encoding="ascii",
        )
        for directory in (path for path in staging.rglob("*") if path.is_dir()):
            directory.chmod(PRIVATE_DIRECTORY_MODE)
        os.rename(staging, final)
        final.chmod(PRIVATE_DIRECTORY_MODE)
    except BaseException:
        if staging.exists() and staging.is_dir() and staging.parent == output_root:
            shutil.rmtree(staging)
        raise
    return final


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument(
        "--output-root",
        type=Path,
        default=DEFAULT_OUTPUT_ROOT,
        help="private parent directory for a new versioned package",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    output = package_release(args.build_dir, args.output_root)
    print(output)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)
