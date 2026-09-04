#!/usr/bin/env python3
"""Generate per-device OTA credentials without tracking private material."""

from __future__ import annotations

import argparse
import hashlib
import secrets
import shutil
import ssl
import subprocess
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SECRETS_DIRECTORY = PROJECT_ROOT / "secrets"
DEFAULT_CERTIFICATE = PROJECT_ROOT / "main" / "ota_server_cert.pem"


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--secrets-dir",
        type=Path,
        default=DEFAULT_SECRETS_DIRECTORY,
        help=f"private output directory (default: {DEFAULT_SECRETS_DIRECTORY})",
    )
    parser.add_argument(
        "--certificate",
        type=Path,
        default=DEFAULT_CERTIFICATE,
        help=f"public certificate output (default: {DEFAULT_CERTIFICATE})",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="replace existing credentials",
    )
    return parser.parse_args()


def _require_replaceable(paths: list[Path], force: bool) -> None:
    existing = [str(path) for path in paths if path.exists()]
    if existing and not force:
        joined = ", ".join(existing)
        raise FileExistsError(f"credentials already exist: {joined}; use --force")


def main() -> int:
    args = _arguments()
    openssl = shutil.which("openssl")
    if openssl is None:
        raise RuntimeError("openssl is required")

    private_key = args.secrets_dir / "ota_server_key.pem"
    token_file = args.secrets_dir / "ota_token.txt"
    _require_replaceable([private_key, token_file, args.certificate], args.force)

    args.secrets_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
    args.secrets_dir.chmod(0o700)
    args.certificate.parent.mkdir(parents=True, exist_ok=True)

    subprocess.run(
        [
            openssl,
            "ecparam",
            "-name",
            "prime256v1",
            "-genkey",
            "-noout",
            "-out",
            str(private_key),
        ],
        check=True,
    )
    subprocess.run(
        [
            openssl,
            "req",
            "-x509",
            "-new",
            "-sha256",
            "-key",
            str(private_key),
            "-out",
            str(args.certificate),
            "-days",
            "3650",
            "-subj",
            "/CN=esp32-p4-bacnet",
            "-addext",
            "basicConstraints=critical,CA:FALSE",
            "-addext",
            "keyUsage=critical,digitalSignature",
            "-addext",
            "extendedKeyUsage=serverAuth",
            "-addext",
            "subjectAltName=DNS:esp32-p4-bacnet",
        ],
        check=True,
    )

    token_file.write_text(secrets.token_urlsafe(48), encoding="ascii")
    private_key.chmod(0o600)
    token_file.chmod(0o600)

    certificate_der = ssl.PEM_cert_to_DER_cert(
        args.certificate.read_text(encoding="ascii")
    )
    fingerprint = hashlib.sha256(certificate_der).hexdigest()
    print(f"Private key: {private_key} (mode 0600)")
    print(f"Bearer token: {token_file} (mode 0600)")
    print(f"Public certificate: {args.certificate}")
    print(f"Certificate SHA-256: {fingerprint}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
