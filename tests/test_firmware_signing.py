#!/usr/bin/env python3
"""Real OpenSSL signatures and strict Secure Boot v2 wire-format boundaries."""

import hashlib
from pathlib import Path
import stat
import struct
import sys
import tempfile
import unittest
from unittest import mock
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import firmware_signing as signing
import ota_client
from test_ota_tools import _test_firmware_image


def signed_image(image: bytes, private: Path) -> bytes:
    padded = image + b"\xff" * (-len(image) % 4096)
    modulus = int(signing._openssl(["rsa", "-in", str(private), "-modulus", "-noout"]).strip().split(b"=")[1], 16)
    signature = signing._openssl([
        "dgst", "-sha256", "-sign", str(private),
        "-sigopt", "rsa_padding_mode:pss", "-sigopt", "rsa_pss_saltlen:32",
        "-sigopt", "rsa_mgf1_md:sha256",
    ], padded)
    block = (b"\xe7\x02\0\0" + hashlib.sha256(padded).digest()
             + modulus.to_bytes(384, "little") + struct.pack("<I", 65537)
             + pow(2, 6144, modulus).to_bytes(384, "little")
             + struct.pack("<I", (-pow(modulus, -1, 1 << 32)) % (1 << 32))
             + signature[::-1])
    assert len(block) == 1196
    block += struct.pack("<I", zlib.crc32(block)) + bytes(16)
    return padded + block + b"\xff" * (4096 - len(block))


class FirmwareSigningTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temporary.name)
        cls.private, cls.public = cls.root / "secrets/key.pem", cls.root / "public.pem"
        cls.backup = cls.root / "backup"
        signing.generate_key(cls.private, cls.public, cls.backup)
        cls.wrong_private, cls.wrong_public = cls.root / "secrets/wrong.pem", cls.root / "wrong-public.pem"
        signing.generate_key(cls.wrong_private, cls.wrong_public, None)
        cls.unsigned = bytes(_test_firmware_image())
        cls.signed = signed_image(cls.unsigned, cls.private)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def inspect(self, image, key=None, **options):
        path = self.root / "test-image.bin"
        path.write_bytes(image)
        return ota_client._inspect_firmware(path, key or self.public, **options)

    def test_real_signature_and_image_identity(self):
        metadata = self.inspect(self.signed, require_signature=True)
        self.assertTrue(metadata.signed)
        self.assertEqual(metadata.unsigned_size, len(self.unsigned))
        self.assertEqual(metadata.image_sha256, self.unsigned[-32:].hex())
        self.assertEqual(metadata.file_sha256, hashlib.sha256(self.signed).hexdigest())
        self.assertEqual(metadata.signing_key_sha256, hashlib.sha256(self.signed[-4096+36:-4096+812]).hexdigest())
        self.assertNotIn(self.private.read_bytes(), metadata.image)

    def test_unsigned_upload_fails_before_network(self):
        path = self.root / "unsigned.bin"
        path.write_bytes(self.unsigned)
        args = type("Args", (), {"firmware": path})()
        with mock.patch.object(ota_client, "_fetch_status") as fetch:
            with self.assertRaisesRegex(ValueError, "unsigned firmware refused"):
                ota_client._upload(args, "x" * 64)
            fetch.assert_not_called()
        self.assertFalse(self.inspect(self.unsigned).signed)  # legacy inspection remains possible

    def test_wrong_key_is_cryptographically_valid_but_untrusted(self):
        wrong = signed_image(self.unsigned, self.wrong_private)
        self.assertTrue(self.inspect(wrong, self.wrong_public).signed)
        with self.assertRaisesRegex(ValueError, "untrusted signing key"):
            self.inspect(wrong)
        with self.assertRaisesRegex(ValueError, "does not match"):
            signing.check_key(self.wrong_private, self.public)

    def test_enforcing_device_blocks_legacy_escape_and_wrong_local_pin(self):
        metadata = self.inspect(self.signed)
        args = type("Args", (), {
            "firmware": self.root / "candidate.bin", "allow_unsigned_legacy": True,
            "project": ota_client.DEFAULT_PROJECT, "post_cert": None, "cert": self.public,
            "no_wait": True, "host": "192.0.2.1", "port": 443, "timeout": 1,
            "signing_public_key": self.wrong_public,
        })()
        before = {"security": {"software_signature_verification": True},
                  "ota_policy": {"signing_key_sha256": metadata.signing_key_sha256}}
        for candidate in (self.unsigned, signed_image(self.unsigned, self.wrong_private)):
            args.firmware.write_bytes(candidate)
            with mock.patch.object(ota_client, "_fetch_status", return_value=before), mock.patch.object(ota_client, "_connection") as connection:
                with self.assertRaisesRegex(ValueError, "running device's trusted key"):
                    ota_client._upload(args, "x" * 64)
                connection.assert_not_called()

    def test_tampering_and_sector_boundaries(self):
        with self.assertRaises(ValueError):
            self.inspect(self.signed[:-1])
        with self.assertRaises(ValueError):
            self.inspect(self.signed + bytes(4096))
        for position in (len(self.unsigned), -4096, -4096 + 4, -4096 + 1196, -1):
            altered = bytearray(self.signed)
            altered[position] ^= 1
            with self.assertRaises(ValueError):
                self.inspect(altered)
        # Valid ESP hash and block CRC cannot substitute for a real signature.
        altered = bytearray(self.signed)
        altered[-4096 + 812] ^= 1
        struct.pack_into("<I", altered, len(altered) - 4096 + 1196, zlib.crc32(altered[-4096:-4096 + 1196]))
        with self.assertRaisesRegex(ValueError, "signature operation failed"):
            self.inspect(altered)
        altered = bytearray(self.signed)
        altered[20] ^= 1
        end = len(self.unsigned)
        altered[end - 32:end] = hashlib.sha256(altered[:end - 32]).digest()
        with self.assertRaisesRegex(ValueError, "signed image digest mismatch"):
            self.inspect(altered)

    def test_key_permissions_backup_and_no_overwrite(self):
        for path in (self.private, self.backup / "firmware_signing_key.pem"):
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(self.backup.stat().st_mode), 0o700)
        self.assertEqual(self.private.read_bytes(), (self.backup / "firmware_signing_key.pem").read_bytes())
        before = self.private.read_bytes()
        with self.assertRaisesRegex(ValueError, "overwrite"):
            signing.generate_key(self.private, self.public, None)
        self.assertEqual(self.private.read_bytes(), before)
        self.private.chmod(0o644)
        try:
            with self.assertRaisesRegex(ValueError, "0600"):
                signing.check_key(self.private, self.public)
        finally:
            self.private.chmod(0o600)


if __name__ == "__main__":
    unittest.main()
