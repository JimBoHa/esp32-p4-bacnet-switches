#!/usr/bin/env python3
"""Run the required host-only PR checks. Never contacts or flashes a device."""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
REQUIRED_TOOLS = ("git", "cmake", "ctest", "node", "openssl")


def commands() -> list[list[str]]:
    return [
        [sys.executable, "tools/security_audit.py", "--history"],
        ["cmake", "-S", "tests", "-B", "build-tests", "-DCMAKE_BUILD_TYPE=Debug",
         f"-DPython3_EXECUTABLE={sys.executable}"],
        ["cmake", "--build", "build-tests", "--parallel"],
        ["ctest", "--test-dir", "build-tests", "--output-on-failure"],
        [sys.executable, "-m", "unittest", "discover", "-s", "tests", "-p", "test_*.py"],
        ["git", "diff", "--check"],
        ["git", "diff", "--cached", "--check"],
    ]


def preflight() -> None:
    missing = [name for name in REQUIRED_TOOLS if shutil.which(name) is None]
    if missing:
        raise ValueError("missing required host tools: " + ", ".join(missing))
    if sys.version_info < (3, 9) or sys.flags.optimize:
        raise ValueError("use Python 3.9+ without -O/PYTHONOPTIMIZE; test assertions must run")
    version = subprocess.run(["node", "--version"], cwd=ROOT, check=True,
                             capture_output=True, text=True).stdout.strip()
    try:
        major = int(version.lstrip("v").split(".")[0])
    except ValueError as error:
        raise ValueError("cannot determine Node.js version") from error
    if major < 18:
        raise ValueError("Node.js 18+ required; do not silently skip browser runtime tests")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list", action="store_true", help="show commands without running them")
    args = parser.parse_args(argv)
    if args.list:
        for command in commands():
            print(shlex_join(command))
        return 0
    try:
        preflight()
        for index, command in enumerate(commands(), 1):
            print(f"[{index}/7] {shlex_join(command)}", flush=True)
            result = subprocess.run(command, cwd=ROOT, check=False)
            if result.returncode:
                print(f"Host checks failed at stage {index}; nothing deployed.", file=sys.stderr)
                return result.returncode if result.returncode > 0 else 1
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Host test setup failed: {error}", file=sys.stderr)
        return 1
    print("All host checks passed. Hardware, production OTA, and soak tests are separate.")
    return 0


def shlex_join(command: list[str]) -> str:
    import shlex
    return shlex.join(command)


if __name__ == "__main__":
    raise SystemExit(main())
