# Repository instructions for coding agents

Read [docs/TESTING.md](docs/TESTING.md) before updating code or making a PR.
Read [docs/VALIDATION_HISTORY.md](docs/VALIDATION_HISTORY.md) for previous
results and untested boundaries. These instructions also serve as the manual
entry point for Fable/other clients that do not automatically load this file.
Codex's repository instruction mechanism is documented in the
[official AGENTS.md guide](https://developers.openai.com/codex/guides/agents-md/).

## Before work

- Inspect `git status`, current branch, trusted remotes, latest main and open
  PRs. Preserve unrelated user edits. Do not assume a closed/superseded PR is
  missing work or restore discarded/sensitive history.
- Confirm scope: explain/review requests do not authorize writes; code changes
  do not automatically authorize deployment, live negative tests or credential
  rotation. Check for an active hardware soak before any device interaction.
- Target is Waveshare ESP32-P4-POE-ETH, 32 MB, ESP32-P4 revision 0.x/1.x,
  ESP-IDF 5.5.4. Use `docs/HEADER_DIAGNOSTICS.md` and `main/hardware_profile.*`:
  P1 has 40 positions, not a generic ESP32 header. GPIO20/21/22 are physical
  P1-35/34/32. Do not drive pins or change pulls on unknown field wiring.

## Every change / PR

- Add regression coverage for changed behavior and update the runbook when
  commands, dependencies or acceptance boundaries change. Keep tests and
  instructions host-only; do not embed them in the ESP32 image or add runtime
  test tasks/pollers. Docs/test-only changes need no firmware version bump.
- Run `python3 tools/run_host_tests.py` after staging only intended files.
  It runs security/history audit, Debug native ASan/UBSan tests, all Python/Node
  checks and staged/unstaged whitespace checks. Node 18+ is required; do not
  report skipped browser checks or disabled assertions as a full pass.
- Use `.github/pull_request_template.md`. Record tested SHA, commands, separate
  runner counts, failures/retests/skips and hardware tests not run. Preserve
  sanitized release/hardware evidence in `docs/VALIDATION_HISTORY.md`; keep raw
  logs/configuration private. Past evidence does not test a new revision.
- Keep requested features in separate PRs. Both `host-tests` and
  `esp32p4-build` must pass on the final PR head before an authorized merge.
  Do not bypass checks. Recheck main after merge and report its SHA/CI status.
- GitHub HEAD and deployed firmware SHA can differ after host-only/docs changes.
  Check firmware input diffs and state both identities honestly. No incidental
  flash/reboot, key generation or soak restart to make the hashes match.

## Hardware and security boundaries

- Prefer `tools/verify_live_readonly.py` for repeatable public P1/status/report/
  asset checks. It is GET-only and does not require tokens. Use trusted expected
  deployment identity and the correct pinned public certificate. It is not
  full HIL, electrical, visual or known-secret-exclusion acceptance.
- Full HIL sends negative write requests and temporarily consumes COV slots;
  it can change diagnostic counters, and a firmware regression could accept a
  mutation. Use an approved lab window and a private configuration backup.
  Never run it concurrently with a clean soak. Do not use `--expect-inputs-off`
  unless all three inputs are known inactive.
- Never stop an existing soak for a host/docs PR. Interrupted/alerting/short
  runs are not 24-hour passes. Do not disturb other devices or monitoring jobs.
- OTA rejection overwrites the inactive slot; watchdog injection, reboot,
  electrical stimulation and PoE/cable/power tests need explicit maintenance
  authority, stable power and verified private USB recovery. Do not enable
  Secure Boot/flash encryption or burn eFuses as part of routine testing.
- Never print/commit tokens, private keys, raw site reports or production
  binaries/ELFs/maps. Production images embed TLS private key and bearer tokens.
  Print only selected metadata fields, never full `FirmwareMetadata`,
  `dataclasses.asdict(...)`, `.__dict__` or `.image` bytes. The signing private
  key stays host-only. CI uses no production credentials and publishes no image.
- Do not rotate/regenerate deployed credentials or force-push/rewrite history
  without specific authorization. Keep private artifacts in protected ignored
  storage, not PR attachments. Inspect audit failures; never weaken the audit
  merely to make a PR green.
