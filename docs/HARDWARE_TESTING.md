# Hardware acceptance testing

The HIL runner is non-actuating, but not read-only traffic. It validates
independent BACnet discovery, the ordered 27-object model and metadata (the
original 18 positions remain compatible), protocol capability bits,
ReadPropertyMultiple `ALL`, Who-Has/I-Have, confirmed/unconfirmed COV, the
eight-subscription capacity boundary, error paths, mDNS, pinned HTTPS,
authentication rejection, configuration-name protections, runtime diagnostics,
and exact firmware identity. Read [TESTING.md](TESTING.md) for prerequisites,
authorization gates and the separate GET-only checker. Run full HIL in an
approved test window, never during a clean soak.

```sh
python3 -m pip install -r requirements-hil.txt
python3 tools/bacnet_hil_test.py \
  --local-address HOST_IP/24 \
  --device-address DEVICE_IP \
  --device-instance 599152 \
  --mdns-hostname esp32-p4-bacnet \
  --expected-version VERSION \
  --expected-source GIT_REVISION \
  --expected-image-sha256 IMAGE_SHA256 \
  --token-file /secure/path/ota_token.txt \
  --viewer-token-file /secure/path/ota_viewer_token.txt \
  --report /secure/path/hardware-report.json
```

Use a new private report path for every run; raw reports contain site details.
Provide both credential file paths to cover both roles, and inspect explicit
skips as well as pass/fail totals. Add `--expect-inputs-off` only if all three
physical inputs are known inactive, and `--require-time-sync` only with an
approved configured time source. Omitting optional assertions is reduced
coverage, even when the report records no skips.

The local BACnet client binds UDP 47808; coordinate with any existing listener's
owner before testing. The suite temporarily creates and cleans up COV
subscriptions and deliberately probes error/write rejection paths. It does
not drive GPIO or intentionally accept configuration changes, reboot, or
update firmware. However, its denied/invalid HTTP mutation requests can affect
state if the firmware regresses; an unexpectedly accepted name-collision write
triggers a restoration attempt and failure. Keep a private configuration
backup. Error/rate-limit counters can increase and invalidate a soak baseline;
occupied production COV slots can also interfere with capacity tests.

The authenticated line classifier is a separate `input-self-test` command,
not an automatic prerequisite of HIL. It temporarily changes weak pulls;
run only with explicit approval and an electrically suitable setup, initially
with switches/wires disconnected and then with one approved field circuit at
a time. Do not run it on unknown connected wiring. It cannot measure volts
or source current. A metered electrical test must confirm controller-side
3.3 V/high and 0 V/low,
contact continuity, input polarity, and real debounce behavior.

Version 1.19.0's controlled negative OTA test used a separately built image
whose switch task intentionally stopped servicing its watchdog. The task
watchdog reset it, ESP-IDF marked the candidate `aborted`, the bootloader
returned to the exact known-good image, and the persistent log reported
`ota-rolled-back`. The inactive slot was then overwritten with the healthy
image and revalidated. Do not repeat destructive rejection or fault-injection
tests without stable power, a private recovery package, and physical USB access.

After finite acceptance, run the [soak monitor](SOAK_TESTING.md).
