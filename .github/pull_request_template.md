## Change and scope

Describe the change and regression coverage. State whether firmware build
inputs changed or this is host-tools/tests/docs only.

## Test evidence

- Date and exact tested source SHA (include dirty/staged changes, if applicable):
- Host/toolchain versions:
- `python3 tools/run_host_tests.py`: exit status; CTest total; Python total;
  Node runtime executed; security/history and whitespace results:
- GitHub `host-tests` and `esp32p4-build` links/results on the final PR head:
- Live/manual tests and exact options, or **not run** with reason:
- Expected/observed device version, source SHA and app-image SHA-256, if tested:
- Failures, fixes/retests, skips, partial coverage and remaining field checks:
- Soak: not applicable / existing run left intact / requested and actual
  duration, samples, failures, alerts, exit/interruption status:
- Deployment/recovery/configuration cleanup, or **no deployment**:
- Sanitized release/hardware evidence added to `docs/VALIDATION_HISTORY.md`,
  if applicable:

## Review gates

- [ ] Read `AGENTS.md` and `docs/TESTING.md`; added regression tests where needed.
- [ ] Ran the full host command after staging only intended files; no skipped
      browser runtime checks and no disabled assertions.
- [ ] Both CI jobs pass on the final PR revision; new changes require a rerun.
- [ ] No production binaries, raw site reports, tokens, private keys or full
      firmware metadata were included in commits, PR text or artifacts.
- [ ] Live tests stayed within authorization; no incidental flash/reboot,
      credential rotation, pin changes or interruption of an active soak.
- [ ] Hardware gaps and interrupted/alerting soaks are explicit, not passes.
- [ ] Host-only test/documentation changes add no ESP32 runtime work.

After merge, record the merge SHA/main CI result separately. A merged PR is not
evidence that its firmware was deployed; do not claim deployment without
verified running identity.
