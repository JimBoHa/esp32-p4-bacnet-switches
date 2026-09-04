#!/usr/bin/env python3
"""Host tests for OTA credential generation and client-side validation."""

from __future__ import annotations

import importlib.util
import os
import ssl
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
GENERATOR = PROJECT_ROOT / "tools" / "generate_ota_credentials.py"
CLIENT = PROJECT_ROOT / "tools" / "ota_client.py"


def _load_client_module():
    specification = importlib.util.spec_from_file_location("ota_client", CLIENT)
    if specification is None or specification.loader is None:
        raise RuntimeError("could not load ota_client.py")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


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


if __name__ == "__main__":
    os.umask(0o077)
    unittest.main()
