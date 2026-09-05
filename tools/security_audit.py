#!/usr/bin/env python3
"""Fail closed on tracked or reachable repository security-policy violations."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
FORBIDDEN_PATHS = (
    re.compile(r"(?:^|/)(?:firmware_signing_key|secure_boot_signing_key)\.pem$", re.IGNORECASE),
    re.compile(r"(^|/)secrets(/|$)", re.IGNORECASE),
    re.compile(r"(^|/)release/private(/|$)", re.IGNORECASE),
    re.compile(r"(^|/)sdkconfig(?:\.old)?$", re.IGNORECASE),
    re.compile(r"\.(?:bin|elf|map|key|p12|pfx)$", re.IGNORECASE),
    re.compile(r"(?:^|/)(?:ota_server_key|ota_token|ota_viewer_token)\.(?:pem|txt)$", re.IGNORECASE),
    re.compile(r"(?:^|/)(?:hardware-report.*\.json|soak-.*\.jsonl)$", re.IGNORECASE),
)
SECRET_PATTERNS = (
    (
        "private-key PEM",
        re.compile(
            rb"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----\s+"
            rb"[A-Za-z0-9+/=]{40,}"
        ),
    ),
    ("GitHub token", re.compile(rb"gh[pousr]_[A-Za-z0-9]{36,}")),
    (
        "GitHub fine-grained token",
        re.compile(rb"github_pat_[A-Za-z0-9_]{40,}"),
    ),
    ("AWS access key", re.compile(rb"(?:AKIA|ASIA)[A-Z0-9]{16}")),
    (
        "Slack token",
        re.compile(rb"xox[baprs]-[A-Za-z0-9-]{20,}"),
    ),
)
ACTION_REFERENCE = re.compile(
    rb"(?m)^\s*-?\s*uses:\s*([^\s@]+)@([^\s#]+)"
)
CONTAINER_REFERENCE = re.compile(
    rb"(?m)^\s*image:\s*([^\s#]+)"
)
FULL_COMMIT = re.compile(rb"[0-9a-f]{40}\Z")
IMAGE_DIGEST = re.compile(rb"[^@\s]+(?::[^@\s]+)?@sha256:[0-9a-f]{64}\Z")


def _git(*arguments: str, input_data: bytes | None = None) -> bytes:
    result = subprocess.run(
        ["git", *arguments],
        cwd=ROOT,
        input=input_data,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return result.stdout


def _tracked_paths() -> list[str]:
    return [
        value.decode("utf-8", errors="surrogateescape")
        for value in _git("ls-files", "-z").split(b"\0")
        if value
    ]


def _path_findings(path: str) -> list[str]:
    return [
        f"forbidden tracked path ({pattern.pattern}): {path}"
        for pattern in FORBIDDEN_PATHS
        if pattern.search(path)
    ]


def _content_findings(
    path: str, data: bytes, enforce_workflow_policy: bool = True
) -> list[str]:
    findings = [
        f"{name} detected in {path}"
        for name, pattern in SECRET_PATTERNS
        if pattern.search(data)
    ]
    if (
        enforce_workflow_policy
        and path.startswith(".github/workflows/")
        and path.endswith((".yml", ".yaml"))
    ):
        for action, reference in ACTION_REFERENCE.findall(data):
            if not FULL_COMMIT.fullmatch(reference):
                findings.append(
                    "mutable GitHub Action reference in "
                    f"{path}: {action.decode(errors='replace')}"
                )
        for image in CONTAINER_REFERENCE.findall(data):
            if not IMAGE_DIGEST.fullmatch(image):
                findings.append(
                    f"unpinned container image in {path}: "
                    f"{image.decode(errors='replace').split('@', 1)[0]}"
                )
    return findings


def audit_tracked_files() -> list[str]:
    findings: list[str] = []
    for relative in _tracked_paths():
        findings.extend(_path_findings(relative))
        path = ROOT / relative
        if path.is_file():
            findings.extend(_content_findings(relative, path.read_bytes()))
    ignored_examples = (
        "secrets/firmware_signing_key.pem",
        "secrets/ota_token.txt",
        "secrets/ota_viewer_token.txt",
        "secrets/ota_server_key.pem",
        "release/private/v0/test.bin",
        "sdkconfig",
        "build-ci/test.elf",
        "hardware-report-field.json",
        "soak-24h.jsonl",
    )
    for relative in ignored_examples:
        result = subprocess.run(
            ["git", "check-ignore", "--quiet", "--", relative], cwd=ROOT
        )
        if result.returncode != 0:
            findings.append(f"sensitive/generated path is not ignored: {relative}")
    return findings


def audit_history() -> list[str]:
    findings: list[str] = []
    objects: dict[str, str] = {}
    for line in _git("rev-list", "--objects", "--all").splitlines():
        object_id, separator, path = line.partition(b" ")
        if not separator:
            continue
        decoded_path = path.decode("utf-8", errors="surrogateescape")
        objects.setdefault(object_id.decode("ascii"), decoded_path)

    for object_id, path in objects.items():
        object_type = _git("cat-file", "-t", object_id).strip()
        if object_type != b"blob":
            continue
        path_issues = _path_findings(path)
        if path_issues:
            findings.extend(f"history {object_id[:12]}: {issue}" for issue in path_issues)
        data = _git("cat-file", "blob", object_id)
        content_issues = _content_findings(
            path, data, enforce_workflow_policy=False
        )
        findings.extend(
            f"history {object_id[:12]}: {issue}" for issue in content_issues
        )
    return findings


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--history",
        action="store_true",
        help="also inspect every blob reachable from local Git refs",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    findings = audit_tracked_files()
    if args.history:
        findings.extend(audit_history())
    if findings:
        for finding in sorted(set(findings)):
            print(f"FAIL: {finding}", file=sys.stderr)
        return 1
    scope = "tracked files and reachable history" if args.history else "tracked files"
    print(f"Security audit passed: {scope}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"audit error: {error}", file=sys.stderr)
        raise SystemExit(2)
