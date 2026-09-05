#!/usr/bin/env python3
"""Repeat public P1/status/report/asset checks using only pinned HTTPS GETs.

No tokens, BACnet/COV traffic, pin reconfiguration, NVS writes, or deployment.
Raw status/configuration bodies are never printed. Use the separate HIL suite
to test authenticated boundaries and actual credential-value exclusion.
"""

from __future__ import annotations

import argparse
import copy
import http.client
import json
import math
from pathlib import Path
import re
import sys
import time

try:
    from tools import ota_client, bacnet_hil_test
except ImportError:
    import ota_client
    import bacnet_hil_test


ROOT = Path(__file__).resolve().parents[1]
ASSETS = {"/diagnostics": "diagnostics_dashboard.html",
          "/diagnostics/app.js": "diagnostics_dashboard.js",
          "/diagnostics/app.css": "diagnostics_dashboard.css"}


def get(args: argparse.Namespace, route: str) -> bytes:
    connection = ota_client._connection(args.host, args.port, args.cert, args.timeout)
    try:
        connection.request("GET", route, headers={"Accept": "*/*"})
        response = connection.getresponse()
        if response.status != 200:
            raise ValueError(f"{route} returned HTTP {response.status}; expected 200")
        return ota_client._read_response(response)
    finally:
        connection.close()


def validate_status(args: argparse.Namespace, status: object) -> dict:
    if not isinstance(status, dict):
        raise ValueError("status must be a JSON object")
    if (status.get("version") != args.expected_version
        or status.get("project") != ota_client.DEFAULT_PROJECT
        or status.get("state") != "valid"
        or status.get("partition") not in ("ota_0", "ota_1")
        or status.get("access_role") != "anonymous"
        or status.get("image_sha256") != args.expected_image_sha256.lower()
        or not isinstance(status.get("git_revision"), str)
        or not re.fullmatch(r"[0-9a-fA-F]{7,40}", status["git_revision"])
        or not bacnet_hil_test.revision_matches(args.expected_source, status.get("git_revision"))
        or not bacnet_hil_test.header_diagnostics_valid(status.get("header_diagnostics"))):
        raise ValueError("firmware identity, anonymous role, or complete P1 snapshot failed")
    system = status.get("system")
    if (not isinstance(system, dict) or type(system.get("boot_count")) is not int
        or system["boot_count"] < 0):
        raise ValueError("status omitted boot identity")
    return status


def unchanged_fields(status: dict) -> tuple:
    return (status["git_revision"], status["image_sha256"], status.get("partition"),
            status["system"]["boot_count"],
            [(pin["position"], pin["gpio"], pin["pad"], pin["input_enabled_by_diagnostics"],
              pin["initialization_preserved_config"]) for pin in status["header_diagnostics"]["pins"]])


def verify(args: argparse.Namespace) -> dict:
    configuration = {route: json.loads(get(args, route)) for route in ("/config", "/network/config")}
    if not all(isinstance(value, dict) for value in configuration.values()):
        raise ValueError("configuration response must be a JSON object")
    baseline = None
    status = {}
    for index in range(args.samples):
        status = validate_status(args, json.loads(get(args, "/ota/status")))
        fields = unchanged_fields(status)
        if baseline is None:
            baseline = copy.deepcopy(fields)
        elif fields != baseline:
            raise ValueError("boot, image, or pin configuration changed during public reads")
        if index + 1 < args.samples:
            time.sleep(args.interval)
    report = ota_client._validate_diagnostics_report(json.loads(get(args, "/diagnostics/report")), "")
    report_status = validate_status(args, report["status"])
    if unchanged_fields(report_status) != baseline:
        raise ValueError("report boot/image/pin configuration differs from sampled status")
    for route, filename in ASSETS.items():
        if get(args, route) != (ROOT / "main" / filename).read_bytes():
            raise ValueError(f"served asset differs from this checkout: {route}")
    for route, before in configuration.items():
        if json.loads(get(args, route)) != before:
            raise ValueError("configuration changed during read-only verification")
    return {"success": True, "samples": args.samples, "version": args.expected_version,
            "expected_source": args.expected_source, "image_sha256": args.expected_image_sha256.lower(),
            "observed_source": status["git_revision"], "partition": status["partition"],
            "boot_count": status["system"]["boot_count"],
            "positions": 40, "readable_gpios": 25, "configuration_unchanged": True,
            "report_structure_checked": True, "served_assets_match": True,
            "known_token_values_checked": False,
            "high_positions": [{"position": pin["position"], "gpio": pin["gpio"]}
                               for pin in status["header_diagnostics"]["pins"] if pin["raw_level"] is True]}


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=443)
    parser.add_argument("--cert", type=Path, default=ota_client.DEFAULT_CERTIFICATE)
    parser.add_argument("--expected-version", required=True)
    parser.add_argument("--expected-source", required=True)
    parser.add_argument("--expected-image-sha256", required=True)
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--interval", type=float, default=2)
    parser.add_argument("--timeout", type=float, default=5)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if not 2 <= args.samples <= 100 or not 1 <= args.port <= 65535:
        parser.error("samples must be 2-100 and port must be 1-65535")
    if (not math.isfinite(args.interval) or not 0.1 <= args.interval <= 60
        or not math.isfinite(args.timeout) or not 0 < args.timeout <= 60):
        parser.error("interval must be 0.1-60 seconds and timeout must be >0 to 60 seconds")
    if (not re.fullmatch(r"[0-9a-fA-F]{7,40}", args.expected_source)
        or not re.fullmatch(r"[0-9a-fA-F]{64}", args.expected_image_sha256)):
        parser.error("expected source must be clean Git SHA and image SHA-256 must be 64 hex digits")
    try:
        result = verify(args)
    except (OSError, ValueError, http.client.HTTPException) as error:
        print(f"Read-only verification failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
