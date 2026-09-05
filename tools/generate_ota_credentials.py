#!/usr/bin/env python3
"""Generate per-device OTA credentials without tracking private material."""

from __future__ import annotations

import argparse
import hashlib
import os
import secrets
import shutil
import ssl
import subprocess
import sys
import tempfile
from pathlib import Path

from validate_ota_credentials import validate_tokens


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SECRETS_DIRECTORY = PROJECT_ROOT / "secrets"
DEFAULT_CERTIFICATE = PROJECT_ROOT / "main" / "ota_server_cert.pem"
VALIDATOR = PROJECT_ROOT / "tools" / "validate_ota_credentials.py"


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
    parser.add_argument("--viewer-only", action="store_true",
                        help="add/rotate only the viewer token; preserve admin token and TLS identity")
    return parser.parse_args()


def _require_replaceable(paths: list[Path], force: bool) -> None:
    resolved = [path.resolve(strict=False) for path in paths]
    if len(set(resolved)) != len(resolved):
        raise ValueError("private key, token, and certificate outputs must be distinct")
    symlinks = [str(path) for path in paths if path.is_symlink()]
    if symlinks:
        raise ValueError(
            "refusing credential output through symbolic link: "
            + ", ".join(symlinks)
        )
    existing = [str(path) for path in paths if path.exists()]
    if existing and not force:
        joined = ", ".join(existing)
        raise FileExistsError(f"credentials already exist: {joined}; use --force")


def main() -> int:
    args = _arguments()
    private_key = args.secrets_dir / "ota_server_key.pem"
    token_file = args.secrets_dir / "ota_token.txt"
    viewer_token_file = args.secrets_dir / "ota_viewer_token.txt"
    if args.viewer_only:
        validate_tokens(token_file)
        _require_replaceable([viewer_token_file], args.force)
        with tempfile.TemporaryDirectory(prefix=".viewer-token-", dir=args.secrets_dir) as temporary:
            staged_viewer = Path(temporary) / "ota_viewer_token.txt"
            staged_viewer.write_text(secrets.token_urlsafe(48), encoding="ascii")
            staged_viewer.chmod(0o600)
            validate_tokens(token_file, staged_viewer)
            os.replace(staged_viewer, viewer_token_file)
        print(f"Viewer token: {viewer_token_file} (mode 0600); admin/TLS unchanged")
        return 0
    openssl = shutil.which("openssl")
    if openssl is None:
        raise RuntimeError("openssl is required")

    _require_replaceable([private_key, token_file, viewer_token_file, args.certificate], args.force)

    args.secrets_dir.parent.mkdir(parents=True, exist_ok=True)
    args.secrets_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
    args.secrets_dir.chmod(0o700)
    args.certificate.parent.mkdir(parents=True, exist_ok=True)

    if os.stat(args.secrets_dir).st_dev != os.stat(args.certificate.parent).st_dev:
        raise ValueError(
            "secrets and certificate outputs must be on the same filesystem for "
            "replacement-safe installation"
        )

    with tempfile.TemporaryDirectory(
        prefix=".ota-credentials-", dir=args.secrets_dir
    ) as temporary:
        staging = Path(temporary)
        staging.chmod(0o700)
        staged_private_key = staging / "ota_server_key.pem"
        staged_token_file = staging / "ota_token.txt"
        staged_viewer_token_file = staging / "ota_viewer_token.txt"
        staged_certificate = staging / "ota_server_cert.pem"
        subprocess.run(
            [
                openssl,
                "genpkey",
                "-algorithm",
                "EC",
                "-pkeyopt",
                "ec_paramgen_curve:P-256",
                "-out",
                str(staged_private_key),
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
                str(staged_private_key),
                "-out",
                str(staged_certificate),
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
        staged_token_file.write_text(secrets.token_urlsafe(48), encoding="ascii")
        staged_viewer_token_file.write_text(secrets.token_urlsafe(48), encoding="ascii")
        staged_private_key.chmod(0o600)
        staged_token_file.chmod(0o600)
        staged_viewer_token_file.chmod(0o600)
        subprocess.run(
            [
                sys.executable,
                str(VALIDATOR),
                "--certificate",
                str(staged_certificate),
                "--private-key",
                str(staged_private_key),
                "--token-file",
                str(staged_token_file),
                "--viewer-token-file",
                str(staged_viewer_token_file),
            ],
            check=True,
            capture_output=True,
        )
        os.replace(staged_private_key, private_key)
        os.replace(staged_token_file, token_file)
        os.replace(staged_viewer_token_file, viewer_token_file)
        os.replace(staged_certificate, args.certificate)

    private_key.chmod(0o600)
    token_file.chmod(0o600)

    certificate_der = ssl.PEM_cert_to_DER_cert(
        args.certificate.read_text(encoding="ascii")
    )
    fingerprint = hashlib.sha256(certificate_der).hexdigest()
    print(f"Private key: {private_key} (mode 0600)")
    print(f"Admin token: {token_file} (mode 0600)")
    print(f"Viewer token: {viewer_token_file} (mode 0600)")
    print(f"Public certificate: {args.certificate}")
    print(f"Certificate SHA-256: {fingerprint}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
