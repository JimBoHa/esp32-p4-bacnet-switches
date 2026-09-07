#!/usr/bin/env python3
"""Check IP-entry redirects with GET/HEAD only; never sends tokens or follows Location."""

from __future__ import annotations

import argparse
import http.client
import ipaddress
import json
import math
from pathlib import Path
import sys

try:
    from tools import ota_client
except ImportError:
    import ota_client

ROOT = Path(__file__).resolve().parents[1]
HTTP_CASES = (
    ("GET", "/", {}),
    ("HEAD", "/", {}),
    ("GET", "/diagnostics", {}),
    ("GET", "/ota/status", {}),
    ("GET", "/config", {}),
    ("GET", "/network/config", {}),
    ("GET", "/?next=https://untrusted.invalid/", {"Host": "untrusted.invalid:1234"}),
)


def request(args, secure, method, path, headers=None):
    connection = (ota_client._connection(args.host, args.https_port, args.cert, args.timeout)
                  if secure else http.client.HTTPConnection(args.host, args.http_port, timeout=args.timeout))
    try:
        connection.request(method, path, headers=headers or {})
        response = connection.getresponse()
        return response.status, response.getheaders(), ota_client._read_response(response)
    finally:
        connection.close()


def validate_redirect(response, expected_location, plaintext=False):
    status, headers, body = response
    if status != 302 or body:
        raise ValueError("redirect must be HTTP 302 with an empty body")
    expected = {"location": expected_location, "cache-control": "no-store",
                "referrer-policy": "no-referrer", "x-content-type-options": "nosniff"}
    if plaintext:
        expected["connection"] = "close"
    for key, value in expected.items():
        if [item for name, item in headers if name.lower() == key] != [value]:
            raise ValueError(f"redirect header failed: {key}")
    if any(name.lower() in {"set-cookie", "www-authenticate"} for name, _ in headers):
        raise ValueError("redirect must not set credentials or request authentication")


def verify(args):
    # Pin the expected HTTPS device before exercising its plaintext entry point.
    status, _, body = request(args, True, "GET", "/diagnostics")
    if status != 200 or body != (ROOT / "main/diagnostics_dashboard.html").read_bytes():
        raise ValueError("HTTPS dashboard does not match this checkout")
    address = ipaddress.ip_address(args.host)
    if isinstance(address, ipaddress.IPv6Address) and address.ipv4_mapped:
        address = address.ipv4_mapped
    authority = f"[{address}]" if address.version == 6 else str(address)
    port = "" if args.https_port == 443 else f":{args.https_port}"
    expected = f"https://{authority}{port}/diagnostics"
    for method, path, headers in HTTP_CASES:
        validate_redirect(request(args, False, method, path, headers), expected, plaintext=True)
    for method in ("GET", "HEAD"):
        validate_redirect(request(args, True, method, "/"), "/diagnostics")
    return {"success": True, "http_redirect_checks": len(HTTP_CASES),
            "https_root_checks": 2, "dashboard_matches": True,
            "tokens_sent": False, "redirects_followed": False}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="literal device IP, without URL or brackets")
    parser.add_argument("--http-port", type=int, default=80)
    parser.add_argument("--https-port", type=int, default=443)
    parser.add_argument("--cert", type=Path, default=ota_client.DEFAULT_CERTIFICATE)
    parser.add_argument("--timeout", type=float, default=5)
    args = parser.parse_args(argv)
    try:
        address = ipaddress.ip_address(args.host)
        if address.is_unspecified or address.is_multicast or "%" in args.host:
            raise ValueError("use a unicast device IP without an IPv6 zone")
        if isinstance(address, ipaddress.IPv6Address) and address.is_link_local:
            raise ValueError("use the device IPv4 address instead of link-local IPv6")
    except ValueError as error:
        parser.error(str(error))
    if (not 1 <= args.http_port <= 65535 or not 1 <= args.https_port <= 65535
        or args.http_port == args.https_port or not math.isfinite(args.timeout)
        or not 0 < args.timeout <= 60):
        parser.error("use distinct valid ports and timeout >0 to 60 seconds")
    try:
        result = verify(args)
    except (OSError, ValueError, http.client.HTTPException) as error:
        print(f"Redirect verification failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
