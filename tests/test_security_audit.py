#!/usr/bin/env python3
"""Unit tests for repository security-policy detection."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest


PROJECT_ROOT = Path(__file__).resolve().parents[1]
AUDITOR = PROJECT_ROOT / "tools" / "security_audit.py"


def _load_auditor():
    specification = importlib.util.spec_from_file_location("security_audit", AUDITOR)
    if specification is None or specification.loader is None:
        raise RuntimeError("could not load security_audit.py")
    module = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    return module


class SecurityAuditTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.audit = _load_auditor()

    def test_forbidden_paths(self) -> None:
        for path in (
            "secrets/ota_token.txt",
            "other/ota_viewer_token.txt",
            "release/private/v1/firmware.bin",
            "sdkconfig",
            "build/app.elf",
            "field.key",
            "hardware-report-site.json",
            "soak-24h.jsonl",
        ):
            self.assertTrue(self.audit._path_findings(path), path)
        self.assertFalse(self.audit._path_findings("main/ota_server_cert.pem"))
        self.assertFalse(self.audit._path_findings("release/README.md"))

    def test_secret_patterns_do_not_echo_secret(self) -> None:
        private_key = (
            b"-----BEGIN PRIVATE KEY-----\n" + b"A" * 64 + b"\n-----END PRIVATE KEY-----"
        )
        token = b"ghp_" + b"A" * 40
        findings = self.audit._content_findings("notes.txt", private_key + token)
        self.assertEqual(len(findings), 2)
        rendered = "\n".join(findings)
        self.assertNotIn(token.decode(), rendered)
        self.assertNotIn((b"A" * 40).decode(), rendered)

    def test_workflow_requires_immutable_action_and_container_references(self) -> None:
        mutable = b"""
steps:
  - uses: actions/checkout@v7
container:
  image: espressif/idf:v5.5.4
"""
        findings = self.audit._content_findings(
            ".github/workflows/build.yml", mutable
        )
        self.assertEqual(len(findings), 2)

        immutable = (
            b"steps:\n  - uses: actions/checkout@"
            + b"a" * 40
            + b" # v7\ncontainer:\n  image: espressif/idf:v5.5.4@sha256:"
            + b"b" * 64
            + b"\n"
        )
        self.assertFalse(
            self.audit._content_findings(".github/workflows/build.yml", immutable)
        )


if __name__ == "__main__":
    unittest.main()
