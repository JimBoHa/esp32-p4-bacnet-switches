#!/usr/bin/env python3
"""Host tests for OTA credential generation and client-side validation."""

from __future__ import annotations

import importlib.util
import hashlib
import http.server
import json
import os
import ssl
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
GENERATOR = PROJECT_ROOT / "tools" / "generate_ota_credentials.py"
CLIENT = PROJECT_ROOT / "tools" / "ota_client.py"
VALIDATOR = PROJECT_ROOT / "tools" / "validate_ota_credentials.py"


def _load_client_module():
    specification = importlib.util.spec_from_file_location("ota_client", CLIENT)
    if specification is None or specification.loader is None:
        raise RuntimeError("could not load ota_client.py")
    module = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    return module


def _test_firmware_image(
    *,
    chip_id: int = 18,
    flash_size_code: int = 0x50,
    version: bytes = b"1.3.2",
    project: bytes = b"esp32_p4_bacnet_switches",
) -> bytearray:
    header = bytearray(24)
    header[0] = 0xE9
    header[1] = 1
    header[2] = 2  # DIO
    header[3] = flash_size_code | 0x0F  # 80 MHz
    struct.pack_into("<H", header, 12, chip_id)
    header[23] = 1

    segment = bytearray(256)
    struct.pack_into("<I", segment, 0, 0xABCD5432)
    segment[16 : 16 + len(version)] = version
    segment[48 : 48 + len(project)] = project
    image = header + struct.pack("<II", 0x48080020, len(segment)) + segment

    checksum = 0xEF
    for byte in segment:
        checksum ^= byte
    while len(image) % 16 != 15:
        image.append(0)
    image.append(checksum)
    image.extend(hashlib.sha256(image).digest())
    return image


class OtaToolTests(unittest.TestCase):
    def test_generated_credentials_are_valid_private_and_replace_safe(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            secrets_directory = root / "device-secrets"
            certificate = root / "ota_server_cert.pem"
            command = [
                sys.executable,
                str(GENERATOR),
                "--secrets-dir",
                str(secrets_directory),
                "--certificate",
                str(certificate),
            ]
            first = subprocess.run(
                command, check=True, capture_output=True, text=True
            )
            private_key = secrets_directory / "ota_server_key.pem"
            token_file = secrets_directory / "ota_token.txt"
            token = token_file.read_text(encoding="ascii")

            self.assertEqual(stat.S_IMODE(secrets_directory.stat().st_mode), 0o700)
            self.assertEqual(stat.S_IMODE(private_key.stat().st_mode), 0o600)
            self.assertEqual(stat.S_IMODE(token_file.stat().st_mode), 0o600)
            self.assertTrue(
                private_key.read_text(encoding="ascii").startswith(
                    "-----BEGIN PRIVATE KEY-----"
                )
            )
            self.assertTrue(32 <= len(token) <= 128)
            self.assertTrue(all(0x21 <= ord(character) <= 0x7E for character in token))
            self.assertNotIn(token, first.stdout)
            self.assertNotIn(token, first.stderr)
            ssl.PEM_cert_to_DER_cert(certificate.read_text(encoding="ascii"))
            subprocess.run(
                [
                    sys.executable,
                    str(VALIDATOR),
                    "--certificate",
                    str(certificate),
                    "--private-key",
                    str(private_key),
                    "--token-file",
                    str(token_file),
                ],
                check=True,
                capture_output=True,
                text=True,
            )

            key_before = private_key.read_bytes()
            certificate_before = certificate.read_bytes()
            refused = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(refused.returncode, 0)
            self.assertEqual(private_key.read_bytes(), key_before)
            self.assertEqual(certificate.read_bytes(), certificate_before)
            self.assertEqual(token_file.read_text(encoding="ascii"), token)

            replaced = subprocess.run(
                [*command, "--force"], check=True, capture_output=True, text=True
            )
            replacement_token = token_file.read_text(encoding="ascii")
            self.assertNotEqual(replacement_token, token)
            self.assertNotIn(replacement_token, replaced.stdout)
            self.assertNotIn(replacement_token, replaced.stderr)

    def test_validator_rejects_mismatch_bad_format_and_bad_token(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first_secrets = root / "first"
            second_secrets = root / "second"
            certificate = root / "first.pem"
            second_certificate = root / "second.pem"
            for secrets_directory, output_certificate in (
                (first_secrets, certificate),
                (second_secrets, second_certificate),
            ):
                subprocess.run(
                    [
                        sys.executable,
                        str(GENERATOR),
                        "--secrets-dir",
                        str(secrets_directory),
                        "--certificate",
                        str(output_certificate),
                    ],
                    check=True,
                    capture_output=True,
                )

            mismatch = subprocess.run(
                [
                    sys.executable,
                    str(VALIDATOR),
                    "--certificate",
                    str(certificate),
                    "--private-key",
                    str(second_secrets / "ota_server_key.pem"),
                    "--token-file",
                    str(first_secrets / "ota_token.txt"),
                ],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(mismatch.returncode, 0)
            self.assertIn("do not match", mismatch.stdout)

            traditional_key = first_secrets / "traditional.pem"
            subprocess.run(
                [
                    "openssl",
                    "ec",
                    "-in",
                    str(first_secrets / "ota_server_key.pem"),
                    "-out",
                    str(traditional_key),
                ],
                check=True,
                capture_output=True,
            )
            traditional_key.chmod(0o600)
            bad_format = subprocess.run(
                [
                    sys.executable,
                    str(VALIDATOR),
                    "--certificate",
                    str(certificate),
                    "--private-key",
                    str(traditional_key),
                    "--token-file",
                    str(first_secrets / "ota_token.txt"),
                ],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(bad_format.returncode, 0)
            self.assertIn("PKCS#8", bad_format.stdout)

            (first_secrets / "ota_token.txt").write_text(
                "x" * 64 + "\n", encoding="ascii"
            )
            bad_token = subprocess.run(
                [
                    sys.executable,
                    str(VALIDATOR),
                    "--certificate",
                    str(certificate),
                    "--private-key",
                    str(first_secrets / "ota_server_key.pem"),
                    "--token-file",
                    str(first_secrets / "ota_token.txt"),
                ],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(bad_token.returncode, 0)
            self.assertIn("printable ASCII", bad_token.stdout)

    def test_client_rejects_non_printable_tokens(self) -> None:
        client = _load_client_module()
        with tempfile.TemporaryDirectory() as temporary:
            token_file = Path(temporary) / "token.txt"
            token_file.write_text("x" * 64, encoding="ascii")
            self.assertEqual(client._read_token(token_file), "x" * 64)

            token_file.write_text("x" * 32 + "\n" + "y" * 32, encoding="ascii")
            with self.assertRaises(ValueError):
                client._read_token(token_file)

            token_file.write_text("x" * 31, encoding="ascii")
            with self.assertRaises(ValueError):
                client._read_token(token_file)

    def test_client_validates_esp32_p4_image_before_upload(self) -> None:
        client = _load_client_module()
        image = _test_firmware_image()

        with tempfile.TemporaryDirectory() as temporary:
            firmware = Path(temporary) / "firmware.bin"
            firmware.write_bytes(image)
            metadata = client._inspect_firmware(firmware)
            self.assertEqual(metadata.version, "1.3.2")
            self.assertEqual(metadata.project, "esp32_p4_bacnet_switches")
            self.assertEqual(metadata.image_sha256, bytes(image[-32:]).hex())

            corrupted = bytearray(image)
            corrupted[20] ^= 1
            firmware.write_bytes(corrupted)
            with self.assertRaisesRegex(ValueError, "validation hash"):
                client._inspect_firmware(firmware)

            wrong_chip = _test_firmware_image(chip_id=9)
            firmware.write_bytes(wrong_chip)
            with self.assertRaisesRegex(ValueError, "not ESP32-P4"):
                client._inspect_firmware(firmware)

            wrong_flash_size = _test_firmware_image(flash_size_code=0x40)
            firmware.write_bytes(wrong_flash_size)
            with self.assertRaisesRegex(ValueError, "32 MB flash"):
                client._inspect_firmware(firmware)

            wrong_checksum = bytearray(image[:-32])
            wrong_checksum[-1] ^= 1
            wrong_checksum.extend(hashlib.sha256(wrong_checksum).digest())
            firmware.write_bytes(wrong_checksum)
            with self.assertRaisesRegex(ValueError, "image checksum"):
                client._inspect_firmware(firmware)

            malformed_segment = bytearray(image[:-32])
            struct.pack_into("<I", malformed_segment, 28, len(image))
            malformed_segment.extend(hashlib.sha256(malformed_segment).digest())
            firmware.write_bytes(malformed_segment)
            with self.assertRaisesRegex(ValueError, "segment data is truncated"):
                client._inspect_firmware(firmware)

    def test_client_completes_tls_handshake_and_rejects_wrong_pin(self) -> None:
        client = _load_client_module()

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self) -> None:
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b"ok")

            def log_message(self, *_args: object) -> None:
                pass

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            secrets_directory = root / "server"
            certificate = root / "server.pem"
            wrong_certificate = root / "wrong.pem"
            subprocess.run(
                [
                    sys.executable,
                    str(GENERATOR),
                    "--secrets-dir",
                    str(secrets_directory),
                    "--certificate",
                    str(certificate),
                ],
                check=True,
                capture_output=True,
            )
            subprocess.run(
                [
                    sys.executable,
                    str(GENERATOR),
                    "--secrets-dir",
                    str(root / "wrong"),
                    "--certificate",
                    str(wrong_certificate),
                ],
                check=True,
                capture_output=True,
            )

            server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(
                certificate,
                secrets_directory / "ota_server_key.pem",
            )
            server.socket = context.wrap_socket(server.socket, server_side=True)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                connection = client._connection(
                    "127.0.0.1", server.server_port, certificate, 5.0
                )
                connection.request("GET", "/")
                self.assertEqual(connection.getresponse().status, 200)
                connection.close()
                with self.assertRaises(ssl.SSLError):
                    client._connection(
                        "127.0.0.1", server.server_port, wrong_certificate, 5.0
                    )
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=5.0)

    def test_client_uploads_validated_bytes_and_requires_acceptance_hash(self) -> None:
        client = _load_client_module()
        image = _test_firmware_image()

        with tempfile.TemporaryDirectory() as temporary:
            firmware = Path(temporary) / "firmware.bin"
            firmware.write_bytes(image)
            metadata = client._inspect_firmware(firmware)

            class Connection:
                def __init__(self) -> None:
                    self.sent = bytearray()

                def putrequest(self, *_args: object) -> None:
                    pass

                def putheader(self, *_args: object) -> None:
                    pass

                def endheaders(self) -> None:
                    pass

                def send(self, data: bytes) -> None:
                    if not self.sent:
                        firmware.write_bytes(b"changed after validation")
                    self.sent.extend(data)

                def getresponse(self) -> object:
                    return object()

                def close(self) -> None:
                    pass

            arguments = type(
                "Arguments",
                (),
                {
                    "firmware": firmware,
                    "project": "esp32_p4_bacnet_switches",
                    "post_cert": None,
                    "cert": Path("certificate.pem"),
                    "no_wait": True,
                    "post_token_file": None,
                    "host": "192.0.2.1",
                    "port": 443,
                    "timeout": 1.0,
                },
            )()
            connection = Connection()
            accepted = {
                "accepted": True,
                "image_sha256": metadata.image_sha256,
            }
            with mock.patch.object(client, "_connection", return_value=connection), \
                    mock.patch.object(client, "_show_response", return_value=(0, accepted)):
                self.assertEqual(client._upload(arguments, "x" * 64), 0)
            self.assertEqual(connection.sent, image)

            firmware.write_bytes(image)
            with mock.patch.object(client, "_connection", return_value=Connection()), \
                    mock.patch.object(
                        client,
                        "_show_response",
                        return_value=(0, {"accepted": True}),
                    ):
                with self.assertRaisesRegex(ValueError, "accepted image hash"):
                    client._upload(arguments, "x" * 64)

    def test_client_gets_and_puts_persistent_configuration(self) -> None:
        client = _load_client_module()

        class Response:
            status = 202
            reason = "Accepted"

            def read(self) -> bytes:
                return b'{"accepted":true,"restart_required":true}'

        class Connection:
            def __init__(self) -> None:
                self.requests: list[tuple[object, ...]] = []
                self.closed = False

            def request(self, *args: object, **kwargs: object) -> None:
                self.requests.append((*args, kwargs))

            def getresponse(self) -> Response:
                return Response()

            def close(self) -> None:
                self.closed = True

        with tempfile.TemporaryDirectory() as temporary:
            configuration = Path(temporary) / "config.json"
            configuration.write_text(
                json.dumps({"schema": 1, "device_instance": 599152}),
                encoding="utf-8",
            )
            arguments = type(
                "Arguments",
                (),
                {
                    "configuration": configuration,
                    "host": "192.0.2.1",
                    "port": 443,
                    "cert": Path("certificate.pem"),
                    "timeout": 1.0,
                    "output": Path(temporary) / "exported.json",
                },
            )()

            get_connection = Connection()
            with mock.patch.object(
                client, "_connection", return_value=get_connection
            ), mock.patch("builtins.print"):
                self.assertEqual(client._config_get(arguments, "x" * 64), 0)
            self.assertTrue(get_connection.closed)
            self.assertEqual(get_connection.requests[0][0:2], ("GET", "/config"))
            get_headers = get_connection.requests[0][-1]["headers"]
            self.assertEqual(get_headers["Authorization"], "Bearer " + "x" * 64)
            self.assertTrue(arguments.output.is_file())
            self.assertTrue(json.loads(arguments.output.read_text())["accepted"])

            put_connection = Connection()
            with mock.patch.object(
                client, "_connection", return_value=put_connection
            ), mock.patch("builtins.print"):
                self.assertEqual(client._config_put(arguments, "y" * 64), 0)
            self.assertTrue(put_connection.closed)
            self.assertEqual(put_connection.requests[0][0:2], ("PUT", "/config"))
            request_arguments = put_connection.requests[0][-1]
            self.assertEqual(
                request_arguments["headers"]["Content-Type"], "application/json"
            )
            self.assertEqual(
                json.loads(request_arguments["body"]),
                {"schema": 1, "device_instance": 599152},
            )

            configuration.write_text("[]", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "JSON object"):
                client._config_put(arguments, "x" * 64)
            configuration.write_text("{", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "not valid JSON"):
                client._config_put(arguments, "x" * 64)

    def test_client_waits_for_reboot_and_rollback_validation(self) -> None:
        client = _load_client_module()
        metadata = client.FirmwareMetadata(
            size=123,
            file_sha256="a" * 64,
            image_sha256="b" * 64,
            project="esp32_p4_bacnet_switches",
            version="1.3.2",
        )
        arguments = type(
            "Arguments",
            (),
            {
                "host": "192.0.2.1",
                "port": 443,
                "timeout": 1.0,
                "reboot_timeout": 10.0,
                "poll_interval": 0.01,
            },
        )()
        before = {"partition": "ota_0", "system": {"boot_count": 7}}
        pending = {
            "partition": "ota_1",
            "state": "pending-verify",
            "image_sha256": "b" * 64,
            "version": "1.3.2",
            "system": {"boot_count": 8},
        }
        valid = {**pending, "state": "valid"}
        with mock.patch.object(
            client, "_fetch_status", side_effect=[pending, valid]
        ), mock.patch.object(client.time, "sleep"):
            self.assertEqual(
                client._wait_for_deployment(
                    arguments,
                    Path("certificate.pem"),
                    "x" * 64,
                    metadata,
                    before,
                ),
                0,
            )

        rolled_back = {
            "partition": "ota_0",
            "state": "valid",
            "image_sha256": "c" * 64,
            "system": {"boot_count": 9},
        }
        with mock.patch.object(
            client, "_fetch_status", return_value=rolled_back
        ), mock.patch.object(client.time, "sleep"):
            with self.assertRaisesRegex(ValueError, "different image"):
                client._wait_for_deployment(
                    arguments,
                    Path("certificate.pem"),
                    "x" * 64,
                    metadata,
                    before,
                )


if __name__ == "__main__":
    os.umask(0o077)
    unittest.main()
