#!/usr/bin/env python3
"""Pinned-certificate client for the ESP32-P4 authenticated HTTPS OTA API."""

from __future__ import annotations

import argparse
import getpass
import hashlib
import hmac
import http.client
import json
import ssl
import sys
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CERTIFICATE = PROJECT_ROOT / "main" / "ota_server_cert.pem"
DEFAULT_TOKEN_FILE = PROJECT_ROOT / "secrets" / "ota_token.txt"
DEFAULT_PROJECT = "esp32_p4_bacnet_switches"


def _read_token(path: Path | None) -> str:
    if path is not None:
        try:
            token = path.read_text(encoding="ascii").strip()
        except FileNotFoundError:
            token = ""
    else:
        token = ""
    if not token:
        token = getpass.getpass("OTA bearer token: ").strip()
    if not 32 <= len(token) <= 128 or any(
        not 0x21 <= ord(character) <= 0x7E for character in token
    ):
        raise ValueError("OTA token must contain 32-128 printable ASCII characters")
    return token


def _certificate_der(path: Path) -> bytes:
    pem = path.read_text(encoding="ascii")
    return ssl.PEM_cert_to_DER_cert(pem)


def _connection(
    host: str,
    port: int,
    certificate: Path,
    timeout: float,
) -> http.client.HTTPSConnection:
    expected_der = _certificate_der(certificate)
    context = ssl.create_default_context(cafile=str(certificate))
    context.check_hostname = False
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    connection = http.client.HTTPSConnection(
        host, port=port, timeout=timeout, context=context
    )
    connection.connect()
    assert connection.sock is not None
    peer_der = connection.sock.getpeercert(binary_form=True)
    if not hmac.compare_digest(
        hashlib.sha256(peer_der).digest(),
        hashlib.sha256(expected_der).digest(),
    ):
        connection.close()
        raise ssl.SSLCertVerificationError(
            "device certificate does not match the pinned certificate"
        )
    return connection


def _show_response(response: http.client.HTTPResponse) -> int:
    payload = response.read().decode("utf-8", errors="replace")
    print(f"HTTP {response.status} {response.reason}")
    try:
        print(json.dumps(json.loads(payload), indent=2, sort_keys=True))
    except json.JSONDecodeError:
        if payload:
            print(payload)
    return 0 if 200 <= response.status < 300 else 1


def _status(args: argparse.Namespace, token: str) -> int:
    connection = _connection(args.host, args.port, args.cert, args.timeout)
    try:
        connection.request(
            "GET",
            "/ota/status",
            headers={"Authorization": f"Bearer {token}", "Accept": "application/json"},
        )
        return _show_response(connection.getresponse())
    finally:
        connection.close()


def _upload(args: argparse.Namespace, token: str) -> int:
    firmware: Path = args.firmware
    size = firmware.stat().st_size
    if size <= 0:
        raise ValueError("firmware image is empty")

    digest = hashlib.sha256()
    with firmware.open("rb") as source:
        for chunk in iter(lambda: source.read(65536), b""):
            digest.update(chunk)
    print(f"Uploading {firmware} ({size} bytes, SHA-256 {digest.hexdigest()})")

    connection = _connection(args.host, args.port, args.cert, args.timeout)
    try:
        connection.putrequest("POST", "/ota")
        connection.putheader("Authorization", f"Bearer {token}")
        connection.putheader("Content-Type", "application/octet-stream")
        connection.putheader("Content-Length", str(size))
        connection.putheader("X-Firmware-Project", args.project)
        connection.putheader("Accept", "application/json")
        connection.endheaders()
        with firmware.open("rb") as source:
            for chunk in iter(lambda: source.read(16384), b""):
                connection.send(chunk)
        return _show_response(connection.getresponse())
    finally:
        connection.close()


def _input_self_test(args: argparse.Namespace, token: str) -> int:
    connection = _connection(args.host, args.port, args.cert, args.timeout)
    try:
        connection.request(
            "POST",
            "/diagnostics/input-self-test",
            body=b"",
            headers={
                "Authorization": f"Bearer {token}",
                "Content-Length": "0",
                "Accept": "application/json",
            },
        )
        return _show_response(connection.getresponse())
    finally:
        connection.close()


def _add_connection_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--host", required=True, help="device IPv4 address or hostname")
    parser.add_argument("--port", type=int, default=443, help="HTTPS port (default: 443)")
    parser.add_argument(
        "--cert",
        type=Path,
        default=DEFAULT_CERTIFICATE,
        help=f"pinned device certificate (default: {DEFAULT_CERTIFICATE})",
    )
    parser.add_argument(
        "--token-file",
        type=Path,
        default=DEFAULT_TOKEN_FILE,
        help=f"bearer-token file (default: {DEFAULT_TOKEN_FILE})",
    )
    parser.add_argument(
        "--timeout", type=float, default=300.0, help="network timeout in seconds"
    )


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)

    status = commands.add_parser("status", help="read authenticated OTA status")
    _add_connection_arguments(status)

    self_test = commands.add_parser(
        "input-self-test",
        help="test GPIO weak-pull response with all field wiring disconnected",
    )
    _add_connection_arguments(self_test)

    upload = commands.add_parser("upload", help="upload an ESP-IDF application image")
    _add_connection_arguments(upload)
    upload.add_argument("firmware", type=Path, help="application .bin from idf.py build")
    upload.add_argument(
        "--project",
        default=DEFAULT_PROJECT,
        help=f"required firmware project name (default: {DEFAULT_PROJECT})",
    )
    return parser.parse_args()


def main() -> int:
    args = _arguments()
    try:
        if not args.cert.is_file():
            raise FileNotFoundError(f"certificate not found: {args.cert}")
        token = _read_token(args.token_file)
        if args.command == "status":
            return _status(args, token)
        if args.command == "input-self-test":
            return _input_self_test(args, token)
        if not args.firmware.is_file():
            raise FileNotFoundError(f"firmware not found: {args.firmware}")
        return _upload(args, token)
    except (OSError, ValueError, ssl.SSLError, http.client.HTTPException) as error:
        print(f"OTA client error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
