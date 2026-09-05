#!/usr/bin/env python3
"""Exercise recoverable OTA rejection paths without selecting a new boot image."""

from __future__ import annotations

import argparse
import hashlib
import http.client
import struct
import sys
import time
from pathlib import Path
from typing import Callable

import ota_client


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_FIRMWARE = PROJECT_ROOT / "build" / "esp32_p4_bacnet_switches.bin"
APP_DESCRIPTION_PROJECT_OFFSET = 48
APP_DESCRIPTION_PROJECT_BYTES = 32


def _rewrite_project(image: bytes, project: str) -> bytes:
    try:
        encoded_project = project.encode("ascii")
    except UnicodeEncodeError as error:
        raise ValueError("replacement project must be ASCII") from error
    if not encoded_project or len(encoded_project) >= APP_DESCRIPTION_PROJECT_BYTES:
        raise ValueError("replacement project must contain 1-31 ASCII bytes")
    if len(image) < ota_client.ESP_IMAGE_HEADER_BYTES + 32:
        raise ValueError("firmware image is truncated")

    rewritten = bytearray(image)
    segment_count = rewritten[1]
    digest_offset = len(rewritten) - hashlib.sha256().digest_size
    position = ota_client.ESP_IMAGE_HEADER_BYTES
    descriptor_offset = 0
    checksum = ota_client.ESP_IMAGE_CHECKSUM_MAGIC
    for segment_index in range(segment_count):
        if position + ota_client.ESP_SEGMENT_HEADER_BYTES > digest_offset:
            raise ValueError("firmware segment header is truncated")
        segment_size = struct.unpack_from("<I", rewritten, position + 4)[0]
        segment_start = position + ota_client.ESP_SEGMENT_HEADER_BYTES
        segment_end = segment_start + segment_size
        if segment_end > digest_offset:
            raise ValueError("firmware segment data is truncated")
        if segment_index == 0:
            descriptor_offset = segment_start
        position = segment_end

    project_start = descriptor_offset + APP_DESCRIPTION_PROJECT_OFFSET
    project_end = project_start + APP_DESCRIPTION_PROJECT_BYTES
    if descriptor_offset == 0 or project_end > digest_offset:
        raise ValueError("firmware application descriptor is truncated")
    rewritten[project_start:project_end] = b"\0" * APP_DESCRIPTION_PROJECT_BYTES
    rewritten[project_start : project_start + len(encoded_project)] = encoded_project

    position = ota_client.ESP_IMAGE_HEADER_BYTES
    for _ in range(segment_count):
        segment_size = struct.unpack_from("<I", rewritten, position + 4)[0]
        segment_start = position + ota_client.ESP_SEGMENT_HEADER_BYTES
        segment_end = segment_start + segment_size
        for byte in rewritten[segment_start:segment_end]:
            checksum ^= byte
        position = segment_end
    checksum_offset = position + (15 - (position % 16))
    if checksum_offset + 1 != digest_offset:
        raise ValueError("firmware checksum position is invalid")
    rewritten[checksum_offset] = checksum
    rewritten[digest_offset:] = hashlib.sha256(rewritten[:digest_offset]).digest()
    return bytes(rewritten)


def _post(
    args: argparse.Namespace,
    token: str,
    *,
    body: bytes,
    content_length: int,
    content_type: str,
    project: str,
) -> tuple[int, str]:
    connection = ota_client._connection(
        args.host, args.port, args.cert, args.timeout
    )
    try:
        connection.putrequest("POST", "/ota")
        connection.putheader("Authorization", f"Bearer {token}")
        connection.putheader("Content-Type", content_type)
        connection.putheader("Content-Length", str(content_length))
        connection.putheader("X-Firmware-Project", project)
        connection.putheader("Accept", "application/json")
        connection.endheaders()
        if body:
            connection.send(body)
        response = connection.getresponse()
        payload = ota_client._read_response(response).decode(
            "utf-8", errors="replace"
        )
        return response.status, payload
    finally:
        connection.close()


def _identity(status: dict[str, object]) -> tuple[object, object, object, object]:
    system = status.get("system")
    boot_count = system.get("boot_count") if isinstance(system, dict) else None
    return (
        status.get("project"),
        status.get("partition"),
        status.get("image_sha256"),
        boot_count,
    )


def _wait_for_unchanged(
    args: argparse.Namespace,
    token: str,
    baseline_identity: tuple[object, object, object, object],
) -> dict[str, object]:
    deadline = time.monotonic() + args.settle_timeout
    last_error = "device did not respond"
    while time.monotonic() < deadline:
        try:
            status = ota_client._fetch_status(
                args.host, args.port, args.cert, args.timeout, token
            )
        except (OSError, ValueError, http.client.HTTPException) as error:
            last_error = str(error)
            time.sleep(0.25)
            continue
        if _identity(status) != baseline_identity:
            raise ValueError("failed OTA changed running firmware identity")
        if status.get("state") != "valid":
            raise ValueError(
                f"running firmware state changed to {status.get('state')!r}"
            )
        return status
    raise TimeoutError(
        f"device did not recover within {args.settle_timeout:.0f} seconds: "
        f"{last_error}"
    )


def _expect_rejection(
    name: str,
    expected_status: int,
    operation: Callable[[], tuple[int, str]],
) -> None:
    status, payload = operation()
    if status != expected_status:
        raise ValueError(
            f"{name} returned HTTP {status}, expected {expected_status}: {payload}"
        )
    print(f"[PASS] {name}: HTTP {status}")


def _expect_rejection_or_close(
    name: str,
    expected_status: int,
    operation: Callable[[], tuple[int, str]],
) -> None:
    try:
        _expect_rejection(name, expected_status, operation)
    except http.client.RemoteDisconnected:
        print(f"[PASS] {name}: connection closed fail-safe before a response")


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="device IPv4 address or hostname")
    parser.add_argument("--port", type=int, default=443)
    parser.add_argument("--cert", type=Path, default=ota_client.DEFAULT_CERTIFICATE)
    parser.add_argument(
        "--token-file", type=Path, default=ota_client.DEFAULT_TOKEN_FILE
    )
    parser.add_argument("--firmware", type=Path, default=DEFAULT_FIRMWARE)
    parser.add_argument("--wrong-key-firmware", type=Path,
                        help="optional valid SBv2 image signed with a disposable untrusted key")
    parser.add_argument("--wrong-key-public-key", type=Path,
                        help="disposable public key proving the wrong-key image is correctly signed")
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--settle-timeout", type=float, default=30.0)
    parser.add_argument(
        "--confirm-inactive-slot-overwrite",
        action="store_true",
        help="required acknowledgement that tests erase the inactive OTA slot",
    )
    return parser.parse_args()


def run(args: argparse.Namespace) -> int:
    if not args.confirm_inactive_slot_overwrite:
        raise ValueError(
            "refusing to erase the inactive OTA slot without "
            "--confirm-inactive-slot-overwrite"
        )
    if not args.cert.is_file() or not args.firmware.is_file():
        raise FileNotFoundError("certificate or firmware image was not found")
    if not 1 <= args.port <= 65535 or args.timeout <= 0 or args.settle_timeout <= 0:
        raise ValueError("port and timeouts are invalid")

    token = ota_client._read_token(args.token_file)
    metadata = ota_client._inspect_firmware(args.firmware)
    wrong_key_path = getattr(args, "wrong_key_firmware", None)
    wrong_key_public = getattr(args, "wrong_key_public_key", None)
    if bool(wrong_key_path) != bool(wrong_key_public):
        raise ValueError("both wrong-key firmware and its public key are required")
    wrong_key = ota_client._inspect_firmware(
        wrong_key_path, wrong_key_public, require_signature=True,
    ) if wrong_key_path is not None else None
    baseline = ota_client._fetch_status(
        args.host, args.port, args.cert, args.timeout, token
    )
    baseline_identity = _identity(baseline)
    if baseline.get("state") != "valid" or baseline.get("project") != metadata.project:
        raise ValueError("device is not a validated member of the firmware project")
    if wrong_key is not None and (
        not metadata.signed or wrong_key.project != metadata.project
        or wrong_key.signing_key_sha256 == metadata.signing_key_sha256
        or baseline.get("ota_policy", {}).get("signing_key_sha256") != metadata.signing_key_sha256
    ):
        raise ValueError("wrong-key tests require signature enforcement and an independently signed project image")

    print(
        "WARNING: rejection tests overwrite only the inactive OTA slot; "
        "install a valid update afterward to restore rollback redundancy."
    )
    common = {
        "args": args,
        "token": token,
        "body": b"",
        "content_length": 0,
        "content_type": "application/octet-stream",
        "project": metadata.project,
    }
    _expect_rejection(
        "wrong media type",
        415,
        lambda: _post(**{**common, "content_type": "text/plain"}),
    )
    _expect_rejection(
        "wrong project header",
        400,
        lambda: _post(**{**common, "project": "wrong_project"}),
    )
    _expect_rejection(
        "missing firmware body",
        411,
        lambda: _post(**common),
    )
    tiny = b"x" * 64
    _expect_rejection(
        "tiny firmware image",
        400,
        lambda: _post(
            **{**common, "body": tiny, "content_length": len(tiny)}
        ),
    )
    _expect_rejection_or_close(
        "oversized firmware declaration",
        413,
        lambda: _post(
            **{**common, "content_length": ota_client.MAX_OTA_IMAGE_BYTES + 1}
        ),
    )
    _wait_for_unchanged(args, token, baseline_identity)
    invalid_image = b"x" * 512
    _expect_rejection(
        "invalid ESP image",
        400,
        lambda: _post(
            **{
                **common,
                "body": invalid_image,
                "content_length": len(invalid_image),
            }
        ),
    )

    connection = ota_client._connection(
        args.host, args.port, args.cert, args.timeout
    )
    try:
        connection.putrequest("POST", "/ota")
        connection.putheader("Authorization", f"Bearer {token}")
        connection.putheader("Content-Type", "application/octet-stream")
        connection.putheader("Content-Length", str(metadata.size))
        connection.putheader("X-Firmware-Project", metadata.project)
        connection.endheaders()
        connection.send(metadata.image[:1024])
    finally:
        connection.close()
    _wait_for_unchanged(args, token, baseline_identity)
    print("[PASS] interrupted firmware upload: running image unchanged")

    if metadata.signed:
        import struct
        import zlib
        tampered = bytearray(metadata.image)
        offset = len(tampered) - 4096
        tampered[offset + 812] ^= 1  # Only signature changes; image hash remains valid.
        struct.pack_into("<I", tampered, offset + 1196, zlib.crc32(tampered[offset:offset + 1196]))
        cases = [("unsigned image", metadata.image[:metadata.unsigned_size]),
                 ("tampered signature with valid block CRC", bytes(tampered))]
        if wrong_key is not None:
            cases.append(("cryptographically valid but untrusted signing key", wrong_key.image))
        for name, body in cases:
            _expect_rejection(name, 400, lambda body=body: _post(
                **{**common, "body": body, "content_length": len(body)}))
            _wait_for_unchanged(args, token, baseline_identity)

    wrong_project_image = _rewrite_project(metadata.image[:metadata.unsigned_size], "wrong_project")
    _expect_rejection(
        "wrong-project ESP image (unsigned)",
        400,
        lambda: _post(
            **{
                **common,
                "body": wrong_project_image,
                "content_length": len(wrong_project_image),
            }
        ),
    )
    final_status = _wait_for_unchanged(args, token, baseline_identity)
    if final_status.get("state") != "valid":
        raise ValueError("running image is no longer valid")
    print("[PASS] all OTA rejection paths preserved the running image")
    return 0


def main() -> int:
    try:
        return run(_arguments())
    except (
        OSError,
        TimeoutError,
        UnicodeError,
        ValueError,
        http.client.HTTPException,
    ) as error:
        print(f"OTA rejection test error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
