# Test runbook for contributors and coding agents

Start here when updating code, reviewing a PR, or resuming a Codex/Fable
session. Read [AGENTS.md](../AGENTS.md) for repository rules and
[VALIDATION_HISTORY.md](VALIDATION_HISTORY.md) for dated evidence. Past passes
are not a substitute for testing the revision you change.

## Host-only by design

`tests/`, the Python tools, this runbook, validation history, and PR instructions
run on a workstation or GitHub Actions. They are not added to the ESP-IDF
component sources, embedded assets, startup tasks, polling loops, or flash
partitions. No test code, persistent background load or storage is added to
the firmware. The host suite never contacts the board. Optional live checks
below generate ordinary network requests and must respect the device's load
and maintenance limits. The C tests compile selected production modules into
separate host executables; they do not install a test framework on the board.

Do not flash, reboot, bump the firmware version, or restart an endurance run
just because host tests or documentation changed. GitHub HEAD can legitimately
be newer than the running firmware's source revision. Report both revisions and
check whether firmware build inputs changed; never label an old image as built
from a newer documentation-only commit.

## Required checks for every PR

Prerequisites: Python 3.9+, Git, CMake 3.20+, CTest, a C11 compiler with Address
and Undefined Behavior Sanitizers (Clang/GCC), OpenSSL CLI, and Node.js 18+.
Node is required: a skipped JavaScript runtime test is not a full host pass.
Do not use Python `-O`/`PYTHONOPTIMIZE` or a Release build that disables C
assertions. ESP-IDF, production credentials, BACpypes, and a live device are
not needed for the host suite.

From the repository root:

```sh
python3 tools/run_host_tests.py
```

This is also the GitHub Actions `host-tests` command. It stops on the first
failed stage and runs:

1. Security audit of tracked files and available Git history.
2. Debug CMake configuration in ignored `build-tests/` using the invoking Python.
3. Host C build with warnings-as-errors and ASan/UBSan.
4. All CTests, including native firmware-model tests and selected Python suites.
5. All `tests/test_*.py` unit tests, including Node dashboard execution.
6. Unstaged and staged whitespace/error checks (`git diff --check`).

`python3 tools/run_host_tests.py --list` prints the exact underlying commands.
Some Python tests intentionally run both under CTest and unittest discovery;
report each runner's totals separately, not a misleading sum of unique tests.
Stage only reviewed, intended files and rerun before committing: the security
audit uses Git's tracked-file inventory, so untracked new files are not yet
covered. A shallow clone cannot prove complete history cleanliness; fetch the
normal trusted branch history before claiming the full history audit passed.

Both GitHub jobs must pass on the final PR revision: `host-tests` and
`esp32p4-build`. The latter uses pinned ESP-IDF 5.5.4 and
`tests/sdkconfig.no_ota.defaults`, an ephemeral signing key, and no production
credentials. It checks target compilation and security settings, not live
HTTPS or hardware behavior. It publishes no firmware artifact. See
[DEVELOPMENT.md](DEVELOPMENT.md) for production builds and private packaging.

## Coverage map

| Area | Repeatable evidence |
|---|---|
| BACnet encoding/services, configuration validation, COV retries, debounce/history, clock/log models, OTA authorization/health | `tests/test_bacnet_codec.c` (native ASan/UBSan executable; many internal cases) |
| Actual P1 diagnostics C implementation, board map, preservation/error paths, complete JSON | `tests/test_header_diagnostics.c`, `tests/check_header_json.py`, GPIO stubs |
| Dashboard static/security rules and simulated browser behavior | `tests/test_dashboard_assets.py`, `tests/dashboard_runtime_test.mjs` (Node) |
| OTA clients, signing and key/signature failures, private packaging, credential/security audit | `tests/test_ota_tools.py`, `tests/test_firmware_signing.py`, `tests/test_package_release.py`, `tests/test_security_audit.py` |
| Independent BACnet HIL validators, mDNS parser/probe, soak alerts | `tests/test_bacnet_hil.py`, `tests/test_mdns_probe.py`, `tests/test_soak_monitor.py` |
| Required host runner and public GET-only device verifier | `tests/test_host_runner.py`, `tests/test_live_readonly.py` |
| Agent entry point, documentation links, CI test command, host/build separation | `tests/test_testing_workflow.py` |
| Real BACnet/HTTPS/authentication/COV integration | `tools/bacnet_hil_test.py`; approved lab window required |
| Real public P1/status/report reads and exact served assets | `tools/verify_live_readonly.py`; no token or BACnet traffic |
| OTA rejection and endurance | `tools/ota_rejection_test.py`, `tools/soak_monitor.py`; separate risk gates below |
| Real browser layout/download, trusted NTP, physical input/power and watchdog rollback | Manual protocols below; not replaced by mocks or CI |

When fixing a defect, add a regression test alongside the relevant suite. For
new firmware behavior, cover normal, boundary, malformed-input and unchanged
state cases. Keep synthetic fixtures free of real credentials and site data.
Update this map and the run instructions if a new test dependency is added.

## Choose live checks by risk

Before any live work, confirm the intended device, board model, deployed
version/source/image hash, TLS pin, network interface, wiring, authorization,
and whether another session has a soak running. Never discover a target by
blindly flashing attached hardware. Do not kill an existing test or reuse its
output path. Hardware unavailability is a disclosed test gap, not a pass.

### Public read-only verification

Use a checkout whose dashboard assets match the deployed firmware; newer
host-only/docs changes are fine. Obtain expected identity from the trusted
deployment record or verified private artifact, not just the device under
test. This checker requires the v1.28+ P1 diagnostic schema:

```sh
python3 tools/verify_live_readonly.py \
  --host DEVICE_IP \
  --cert main/ota_server_cert.pem \
  --expected-version VERSION \
  --expected-source DEPLOYED_GIT_SHA \
  --expected-image-sha256 APP_IMAGE_SHA256 \
  --samples 5 --interval 2
```

It uses only certificate-pinned HTTPS GETs: anonymous status, all 40 P1
positions/25 readable GPIOs, unchanged pad/boot/image/configuration fields,
report structure, and byte-identical HTML/JS/CSS. GPIO24/25 remain reserved for
USB; power/ground/control positions have no digital reading. Raw levels may
change between samples; HIGH on a floating or shared-function pin does not
identify a connected switch. Use [HEADER_DIAGNOSTICS.md](HEADER_DIAGNOSTICS.md),
not a generic ESP32 or Pico GPIO numbering diagram.

Output is a small allowlisted summary, not raw status or configuration. It
checks report structure and forbidden credential-field patterns, but has no
tokens and **cannot prove absence of the actual secret values**. The
authenticated HIL suite performs that separate check. This is not a replacement
for BACnet, negative-auth, visual, electrical, or soak testing.

The server has two TLS sockets. Keep requests sequential and avoid a third
concurrent browser/poller. A connection reset is a failed attempt: record it,
investigate concurrency, and record a separate retest rather than discarding
the failure. Ordinary public reads need not restart a soak, but do not add
unnecessary load to an endurance experiment.

### Full hardware acceptance (approved lab window)

Follow [HARDWARE_TESTING.md](HARDWARE_TESTING.md). Install the independent
client in an ignored environment if needed:

```sh
python3 -m venv .cache/hil-venv
. .cache/hil-venv/bin/activate
python3 -m pip install -r requirements-hil.txt
```

`requirements-hil.txt` pins BACpypes3. Use the correct local subnet interface;
the HIL client binds UDP 47808. Check existing listeners and coordinate with
their owner; do not stop another device's monitor or BAS client unilaterally.

Full coverage supplies admin and viewer token **file paths**, expected version,
clean source SHA, app-image SHA, and mDNS hostname. Missing prerequisites can
produce explicit skips; inspect the report, not just the exit status. Use
`--expect-inputs-off` only after confirming all three physical inputs are
inactive. Omit it with active/unknown field wiring. Use `--require-time-sync`
only when an approved reachable time source is configured. A
`--skip-capacity-test` run is partial, not full COV-capacity acceptance.

The HIL suite does not drive GPIO or intentionally accept configuration
changes, but it is **not GET-only**: it sends BACnet error/write probes,
temporarily consumes COV slots, and sends denied/invalid HTTP mutation requests.
An unexpected acceptance can change state; the name-collision test attempts
restoration before failing. Run it with a private configuration backup,
stable connectivity, and approval for negative tests. It can increment
error/rate-limit counters and invalidate a concurrent soak baseline. Never
run it during a clean endurance test or against occupied production COV
capacity without coordination.

### Browser, report download, and physical inputs (manual)

1. Open `/diagnostics` in a clean browser session without a stored token. Check
   automatic loading, refresh, desktop/narrow layouts, 40 P1 rows, and readable
   LOW/HIGH versus explicit reserved/non-GPIO states. Do not paste a token into
   screenshots or the address bar.
2. Download the diagnostics report through the UI into a private directory.
   Verify JSON schema/sections, exact version/source/image identity, 40 P1 rows,
   and absence of credential material locally. Do not publish the raw report.
   The HIL suite checks actual supplied token exclusion; the Node tests and
   served-asset byte match alone do not establish successful native download
   or visual layout. If the browser is unavailable, mark these steps not run.
3. With approved wiring and a meter, correlate one real switch at a time with
   physical P1 position, GPIO raw level, debounced state, BACnet Present_Value,
   and history. Verify intended polarity and debounce. A digital read cannot
   measure volts, prove continuity, or identify a wire by elimination.
4. Weak-pull `input-self-test` is a separate authenticated mutation. Run only
   with permission and an electrically suitable/disconnected test setup; it
   temporarily changes pulls. Compare pad settings, input counters/history,
   and saved configuration before/after. Do not silently run it on unknown
   connected field wiring. See [HARDWARE_TESTING.md](HARDWARE_TESTING.md).

### Trusted time and persistence (manual, controlled test setup)

Use an approved reachable NTP source and the documented build-time setting in
[README.md](../README.md). Changing it requires an authorized firmware build
and deployment. No internet reachability or arbitrary public NTP assumption.

- Before synchronization, confirm time is explicitly unknown and boot-relative
  event timing remains usable. After synchronization, compare UTC to the
  trusted host clock and record observed offset, source and synchronization
  state privately. Run HIL with `--require-time-sync`.
- Create an approved test event, check its captured UTC/boot-relative timing,
  and verify earlier unknown/legacy events are not retrospectively redated.
  With a separately approved reboot, confirm persistent fault records retain
  their timestamps and ordering. Input history is RAM-only and should reset.
- If using a temporary build/server, restore the approved build/configuration,
  remove the temporary service, revalidate exact identity, and record cleanup.
  An unsynchronized deployment is a disclosed limitation, not a positive NTP
  test. Never change system-wide host time to manufacture a pass.

### Signed OTA, negative uploads and watchdog rollback

These are maintenance operations, not normal host/PR checks. Require explicit
deployment/fault-injection authorization, stable power, physical USB recovery,
a verified private recovery package, existing credentials and signing key,
and a known-good rollback image. Do not generate replacement production keys,
rotate credentials, burn eFuses, or enable Secure Boot/flash encryption as a
testing shortcut.

Follow the signed-upload and rejection sections in [README.md](../README.md)
and `python3 tools/ota_rejection_test.py --help`. The rejection tool requires
`--confirm-inactive-slot-overwrite`: it can overwrite the inactive slot even
when the running image is preserved. Cases include invalid media/header/body,
oversize/truncated upload, unsigned image, altered signed image, and an
optionally supplied valid image signed by an untrusted key. Omitting the
wrong-key fixture does not test that case. Record individual outcomes and
exact before/after boot, partition, version, source and app-image identity.
Restore a valid signed candidate and verify healthy rollback redundancy after
negative testing; a successful HTTP rejection alone is insufficient.

For controlled watchdog rollback, first inspect the current health gate and
watchdog configuration. Build a **private, non-release** fault-injection
candidate that stops the intended monitored task from servicing the watchdog
before candidate validation. Verify it never becomes valid, watchdog reset
occurs, the bootloader marks it aborted and returns to the exact known-good
image, and persistent diagnostics record `ota-rolled-back`. Restore/revalidate
healthy images in both slots. Record the exact private test patch, toolchain,
expected deadline and observed reset/rollback; do not improvise a hang on the
working deployment or commit/distribute an enabled fault-injection image.
The historical v1.19 test was manual; no general-purpose automated watchdog
injection fixture is committed.

### Endurance and field-only acceptance

Run finite acceptance **before** starting the [soak monitor](SOAK_TESTING.md).
Use a new private output path and record requested duration/interval, actual
elapsed time, successful/failed samples, alerts and final exit/interruption
state. Only a completed full-duration run with no failures or alerts passes.
An in-progress, interrupted, short, or alerting run is not a 24-hour pass.
Never stop a running soak simply to update this ledger or merge host-only code.

Separately schedule cold boot, cable loss, PoE interruption/brownout, rapid
power cycling, and representative production BAS commissioning with recovery
access and the system owner's approval. These intentional faults do not belong
inside a clean soak. Do not claim field acceptance from host tests or simulated
GPIO fixtures.

## Evidence to commit and evidence to keep private

Use a fresh private directory, for example:

```sh
umask 077
P4_TEST_REPORT_DIR=$(mktemp -d /tmp/p4-validation.XXXXXX)
python3 tools/run_host_tests.py > "$P4_TEST_REPORT_DIR/host-tests.log" 2>&1
```

Check the exit status immediately. Use unique paths for HIL reports and browser
downloads too; never overwrite an earlier run. Keep raw HIL/soak/status/config
reports, serial logs and screenshots private: site addressing, names and
configuration may be sensitive even without a token.

For each PR, fill in [.github/pull_request_template.md](../.github/pull_request_template.md).
Commit a concise sanitized acceptance record to
[VALIDATION_HISTORY.md](VALIDATION_HISTORY.md) for new release/hardware evidence;
for routine host-only changes, the PR's command/results summary and CI links
are sufficient. If validation happens after merge, add a follow-up docs PR for
material release evidence. CI logs and local scratch files are not permanent
records; preserve the useful totals, outcomes, gaps and revision in Git text.

Record date, scope, tested source SHA/dirty state, commands/options (without
secrets/site details), tool versions, runner totals, failures/retests/skips,
expected and observed firmware identity when relevant, CI run/PR links, manual
checks, interrupted-soak status, remaining field work and cleanup. Explain
different check counts when options or test coverage change. Report the PR
head and merge commit separately if they differ; do not claim to have tested
an untested merge revision.

Never commit `secrets/`, production `.bin`/`.elf`/`.map` files, private packages,
token values, private keys or raw hardware reports. Production firmware embeds
the admin/viewer tokens and TLS private key. Print only allowlisted metadata
fields (version, source revision, hashes, signature status); never print a whole
`FirmwareMetadata`, `dataclasses.asdict(...)`, `.__dict__`, or its `.image` bytes.
The RSA signing private key must stay host-only. Do not upload production
firmware as a GitHub Actions artifact or public release attachment.
