# Sanitized validation history

This is durable evidence for future sessions, not a claim that every past
terminal command or private report is available on GitHub. Existing automated
tests are in `tests/`; repeatable procedures are in [TESTING.md](TESTING.md).
The entries below were reconstructed on 2026-09-05 from existing development
notes, local acceptance reports and PR/CI records. Raw reports remain private.
Recorded historical results were **not rerun** merely to write this ledger.

Counts belong to their named runner/options. CTest and Python discovery overlap;
do not sum them as unique test cases. Live image hashes below are the
application descriptor's `image_sha256`, not the whole `.bin` file SHA-256.

## 2026-09-07 — v1.29.0 IP-entry redirect implementation

- Change tree based on `b371ff71d23b33fff1bb2b08ce139fc9127772b5`: HTTPS root
  GET/HEAD redirects to `/diagnostics`; port 80 is a bounded redirect-only
  listener with no management data or mutation handlers. It derives its
  destination from the accepted socket, not Host/path/query. Exact clean
  tested commits, final builds and CI links are recorded in the associated PR.
- `python3 tools/run_host_tests.py`: exit 0, **6/6 CTests**, **90 Python
  tests**, no skips. Native redirect implementation tests run with ASan/UBSan;
  cases cover GET/HEAD, unchanged write authorization, destination isolation,
  custom TLS ports, IPv4/mapped/global IPv6, invalid addresses, startup failure
  cleanup, rejected request bodies, connection closure and resource limits.
  Node, security/history and staged/unstaged whitespace checks passed. Runner
  totals overlap.
- An OTA-enabled ESP-IDF 5.5.4 target build succeeded with the existing private
  credentials/signing key. No key generation or credential changes. Socket
  allowance increased from 10 to 16 for the additional bounded listener;
  existing local `sdkconfig` values must be updated as documented.
- SDK inspection caught that its error-code enum has no 503 constant; the
  implementation uses an explicit 503 status instead, and the native stubs
  were corrected to match the real API before the target build.
- Final source review identified ESP-IDF's automatic unread-body drain after
  successful handlers. Both redirect handlers now reject declared request
  bodies with an empty 400 response and immediate socket closure; native
  tests cover GET/HEAD and small/large lengths. Unsupported methods use the
  SDK's default close-on-error behavior. Receive/send timeouts are per I/O,
  not an absolute deadline for slowly arriving headers.
- Repeatable live acceptance is now committed as
  `tools/verify_dashboard_redirects.py`: seven HTTP redirect cases, two HTTPS
  root cases and the pinned dashboard. It uses GET/HEAD only, sends no tokens,
  and never follows Location. Combine with the existing identity/P1 verifier
  and manual browser navigation after an authorized deployment.
- At preparation of this entry, **no firmware was installed**. Device reboot,
  live redirect tests and browser acceptance await deployment approval; do
  not interpret host/compile passes as a deployed feature. No new soak or
  weak-pull/physical test was run. The v1.28 endurance failures below remain
  unresolved and are not erased by these implementation tests.

## 2026-09-06 — v1.28.0 24-hour soak completed (failed)

This is the completed outcome of the run described as in progress in the
2026-09-05 entries below. It finished naturally while the coding session was
paused; it was not stopped or restarted to prepare this record.

- Firmware: v1.28.0, source `00bebb7ad59c`, app image SHA-256
  `a2c9e24700e0b61ff7929002c5426974fec13c462bdb1173ca14c5beb052ecfd`.
- Procedure: `tools/soak_monitor.py`, requested duration 86,400 seconds,
  interval 60 seconds, timeout 5 seconds, pinned certificate and viewer-token
  file; raw JSONL kept private. UTC start `2026-09-05T22:15:30.640Z`, finish
  `2026-09-06T22:15:31.542Z`; actual elapsed **86,400.470 seconds**.
- Final summary: **success=false**, interrupted=false; **1,441 samples**,
  **1,439 successful requests**, **2 request failures**, **6 alerting samples**.
  A successful request means data was collected, not that its health checks
  passed. The completed duration does not override these failures.
- Both request failures were `/ota/status` connection timeouts, at UTC
  `2026-09-06T01:22:30.666Z` and `2026-09-06T11:54:30.909Z`.
- Counter alerts: one BACnet protocol-error increase (5 to 69), four malformed
  counter increases (0 to 3, 3 to 4, 4 to 5, 5 to 7), and two rate-limit
  increases (2 to 3, 3 to 15). One sample had two counter alerts, hence seven
  counter alerts across six samples. The traffic source and timeout root
  cause have **not been established**; do not dismiss these as expected noise.
- All 1,439 successful snapshots retained the same source/image, valid
  `ota_0` state and boot count 73. Lowest sampled free heap: 33,023,936 bytes;
  final-minus-initial heap: -8 bytes; maximum die temperature: 38.4 C.
  HTTPS latency median/max: 459.079/3,465.401 ms; BACnet median/max:
  1.613/2.390 ms.
- Independently reconciled the JSONL: 1,441 sample rows, one final summary,
  two error rows, six alerting rows, last scheduled sample at 86,400 seconds,
  and one unique successful-snapshot firmware/boot identity.
- No new firmware, configuration change or soak was initiated in response.
  Timeout/traffic investigation and a later clean endurance run remain open
  work, separate from the host-only documentation/test-handoff PR. Physical
  switch, PoE/cable/power and production BAS acceptance remain unclaimed.

## 2026-09-05 — reproducible testing handoff (host-only)

- [PR #36](https://github.com/JimBoHa/esp32-p4-bacnet-switches/pull/36), change
  tree based on `00bebb7ad59c44ef5d50305b0ba839c6e431dec3`; new host
  runner, GET-only verifier, 17 regression tests, agent/PR instructions and
  this evidence ledger. Final clean-head/main SHAs and CI links are recorded
  in the associated PR, separately from this pre-commit acceptance record.
- `python3 tools/run_host_tests.py`: exit 0; 5/5 CTests, 84 Python tests,
  dashboard Node execution, security/history and staged/unstaged whitespace
  checks passed. CTest/Python totals overlap. Local tools: Python 3.9.6,
  CMake 4.2.3, Apple Clang 21.0.0, Node 25.6.1, OpenSSL 3.6.2.
- Initial isolated new-tool unit tests caught a package-style `ota_client`
  import failure; fixed by supporting both package and direct-script imports.
  The 17 new tests and the full suite then passed. This was a host import
  issue, not a device failure.
- Expanded malformed-row subtests then reproduced an uncaught missing-key
  error for omitted nullable P1 fields. The checker now rejects those rows
  with a clear validation failure; the regression matrix and full suite passed
  after the fix. No firmware changes were needed.
- `tools/verify_live_readonly.py` with the v1.28.0 identity below,
  `--samples 5 --interval 2`: passed all 40 P1 positions/25 readable GPIOs,
  anonymous reads, report structure, exact served assets, unchanged
  boot/image/pad/configuration state. No token values were supplied, so actual
  known-secret exclusion was not retested by this command.
- `run_host_tests.py --list` and `verify_live_readonly.py --help` also worked
  from outside the repository directory. Documentation links and exclusion of
  new handoff files from firmware build manifests have regression coverage.
- No firmware build inputs changed; no firmware deployment, reboot, weak-pull
  self-test, full negative HIL, browser visual retest or OTA fault injection.
  The existing v1.28.0 soak remained running and was not restarted. This entry
  does not claim completed 24-hour or field acceptance.

## v1.28.0 — P1 diagnostics and private-package guide

- Changes: [PR #34](https://github.com/JimBoHa/esp32-p4-bacnet-switches/pull/34),
  [PR #35](https://github.com/JimBoHa/esp32-p4-bacnet-switches/pull/35).
- Final tested/deployed source: `00bebb7ad59c44ef5d50305b0ba839c6e431dec3`;
  app image SHA-256:
  `a2c9e24700e0b61ff7929002c5426974fec13c462bdb1173ca14c5beb052ecfd`.
- Host: 5/5 CTests and 67 Python tests passed, including dashboard Node checks.
  Security/history audit and whitespace checks passed. Both CI jobs passed on
  PR and final main; [final-main CI](https://github.com/JimBoHa/esp32-p4-bacnet-switches/actions/runs/33995201761).
- Live: feature and final-main firmware each passed 71/71 HIL checks, no
  declared failures/skips. `--expect-inputs-off` was deliberately omitted
  because field wiring was connected; the optional known-off assertions were
  therefore not executed. No weak-pull input self-test was performed on that
  wiring. This explains the lower total than v1.27, not a lost object.
- Five sequential anonymous P1 polls on each tested image validated all 40
  positions and 25 readable GPIOs, reserved USB/non-GPIO handling, stable pad
  settings and boot/image identity. Configuration/network settings were
  unchanged. The original three input pad configurations were preserved.
- An earlier three-client parallel poll attempt obtained three valid samples
  then a connection reset. Two TLS sockets with LRU purging were a plausible
  cause; the sequential retest passed. The failed attempt is not erased by
  that retest. Current reusable GET-only procedure is in `TESTING.md`.
- Native Safari on the feature build: anonymous automatic load, P1 table,
  refresh and actual report download passed; report had 40 rows and no tested
  token values. Final-main browser window was unavailable, so that visual
  check was **not repeated**; exact served HTML/JS/CSS byte matches and HIL
  passed instead.
- Signed feature and final-main deployments reached valid state. Private
  recovery packaging matched the final application at offset `0x20000`,
  included the P1 guide, and passed hash/permission checks. Signing private-key
  material was absent from app, ELF and merged recovery image.
- Completed feature soak: 120.483 seconds, 5 successful samples, 0 request
  failures, 0 alerts. A separate 24-hour final-main soak was started on
  2026-09-05 and was still in progress when this entry was prepared. **No
  24-hour pass is recorded here**; preserve the active run and append its
  actual outcome later.
- Remaining: electrically correlate the real switch by metering/toggling;
  cold boot/cable/PoE/brownout and production BAS acceptance. Site time was
  unsynchronized without an approved NTP source; positive site NTP acceptance
  is not claimed.
- Additional public evidence: [acceptance comment on PR #34](https://github.com/JimBoHa/esp32-p4-bacnet-switches/pull/34#issuecomment-5555158052).

## v1.27.0 — anonymous read-only dashboard/API

- Change: [PR #33](https://github.com/JimBoHa/esp32-p4-bacnet-switches/pull/33).
  Final tested source: `02b06aacabd5c8bd8033326e4ac7701530056278`.
- Host: 4/4 CTests, 66 Python tests; feature and final-main HIL: 74/74,
  no failures/skips. [PR CI](https://github.com/JimBoHa/esp32-p4-bacnet-switches/actions/runs/33988729597)
  and [main CI](https://github.com/JimBoHa/esp32-p4-bacnet-switches/actions/runs/33989031368)
  passed both jobs.
- Anonymous reads returned 200; all six mutation routes returned 401 without
  authorization and for four invalid-authorization forms. Viewer reads worked;
  all six viewer mutation attempts returned 403. Configuration/network,
  boot/image and self-test state were unchanged after denied requests.
- Weak-pull classification passed 3/3 in that test setup. Feature Safari
  automatic load, refresh/layout and real download passed. Final-main visual
  retest was unavailable; served-asset byte equality and HIL passed.
- Signed feature/final deployments, preserved configuration/credential
  identity, private recovery and absence of signing private-key material from
  artifacts were verified.
- Completed short soak: 120.466 seconds, 5 successful samples, 0 failures or
  alerts. Extended run: 6,840.644 seconds, 115 successful samples, no request
  failures, **one rate-limit-counter alert**, interrupted for the v1.28
  deployment. It did **not** pass a 24-hour soak.

## v1.26.0 — history, report export, roles, NTP, BACnet diagnostics, signing

- Changes: [#27](https://github.com/JimBoHa/esp32-p4-bacnet-switches/pull/27),
  [#28](https://github.com/JimBoHa/esp32-p4-bacnet-switches/pull/28),
  [#29](https://github.com/JimBoHa/esp32-p4-bacnet-switches/pull/29),
  [#30](https://github.com/JimBoHa/esp32-p4-bacnet-switches/pull/30),
  [#31](https://github.com/JimBoHa/esp32-p4-bacnet-switches/pull/31),
  [#32](https://github.com/JimBoHa/esp32-p4-bacnet-switches/pull/32).
  Final tested source: `4bef3f08c31d23ccf2c9f0be16a24996981a05a5`.
- App image SHA-256:
  `e6d851412858bee50cda16d5f3f54740390403ab4cdbf5ad9d399db8aae39824`.
- Host: 4/4 CTests, 65 Python tests; HIL: 72/72, no failures/skips. PR and
  final-main CI passed. All 27 BACnet objects/331 RPM results were checked;
  the first 18 object-list positions remained compatible.
- Weak-pull classification: 3/3 passed with no added input-history transitions
  or rejection counts. Positive NTP matched trusted host UTC; startup unknown
  time and 13 overlapping legacy fault records were preserved. Dated fault
  events survived reboot. Temporary time-test server/build were removed and
  the normal deployment restored.
- Signed OTA migration and subsequent signed updates passed. Unsigned,
  signature-tampered (valid CRC), and cryptographically valid untrusted-key
  candidates were rejected with running boot/image identity preserved.
  Invalid content type/project/header/body, oversize and interrupted upload
  cases were also exercised; the oversized declaration closed the connection
  fail-safe. Healthy signed firmware was restored after negative testing.
- Private recovery matched the exact app. Independent OpenSSL and ESP-IDF
  signature verification passed; signing private-key PEM/DER were absent from
  app, ELF and recovery image. Served assets/DOM and earlier feature Safari
  checks passed; the final signing-label visual check was unavailable.
- Completed short soak: approximately 120 seconds, 5 successful samples,
  0 failures or alerts. Extended run: 6,761.861 seconds, 113 successful samples,
  0 request failures, **one malformed-counter increase (0 to 3) alert**,
  interrupted for v1.27. It did **not** pass a 24-hour soak.

## v1.19.0 — legacy watchdog rollback acceptance

Preserved from the earlier committed `DEVELOPMENT.md` validation note; exact
command logs and source SHA were not in that note, so this entry has a weaker
provenance boundary than the entries above.

- Two complete 53-check HIL runs passed.
- A private switch-task hang candidate caused task-watchdog reset, aborted
  candidate/bootloader rollback to the known-good image, and explicit
  `ota-rolled-back` diagnostics. Healthy images were restored to both slots.
- Eight post-test soak samples completed without failure or alert. Duration
  was not recorded in the note; this is **not** a 24-hour claim.

## Adding an entry

Keep newest evidence first. Include date, change/PR, exact tested source and
dirty state, applicable firmware version/app-image hash, commands/options or
runbook section, toolchain, separate runner totals, CI links, manual checks,
failures/retests/skips, deployment/cleanup and untested boundaries. For soaks,
include requested and actual duration, samples, failures, alerts and final
interruption state. Do not copy raw reports or secret-bearing metadata.

For host-only/docs PRs, use the PR template's durable command/results record;
do not imply a new firmware deployment. Historical entries stay historical:
append corrections or new results instead of silently rewriting an earlier
failure as a pass.
