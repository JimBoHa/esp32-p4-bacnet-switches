# Development and release

## Reproducible baseline

- ESP-IDF: 5.5.4
- Target: `esp32p4`, revision 0.x/1.x family
- Board: Waveshare ESP32-P4-POE-ETH, 32 MB NOR, onboard IP101GRI RMII PHY
- Managed mDNS component: 1.12.0, locked by `dependencies.lock`
- Partition layout: two 4 MiB OTA slots with bootloader rollback

`sdkconfig` and generated components are ignored. `sdkconfig.defaults`,
`partitions.csv`, `main/idf_component.yml`, and `dependencies.lock` are the
committed inputs.

```sh
. /path/to/esp-idf/export.sh
idf.py reconfigure
idf.py build
idf.py size
```

An OTA-enabled build requires the ignored TLS private key and distinct admin/viewer tokens. Generate them for
a new device with `tools/generate_ota_credentials.py`, or point
`ESP32_P4_OTA_SECRETS_DIR` at a protected directory. Never use production
credentials in CI.

Existing deployments can add only the missing viewer token with
`python3 tools/generate_ota_credentials.py --viewer-only`; this preserves the
admin token and TLS identity. Rebuild and upload using the admin credential.

## Tests

Read [TESTING.md](TESTING.md) for prerequisites, coverage, risk gates and PR
evidence. The required host-only command is:

```sh
python3 tools/run_host_tests.py
```

The GitHub workflow has read-only repository permission, pins its action and
ESP-IDF image by immutable digest, disables persisted checkout credentials, and
builds with `tests/sdkconfig.no_ota.defaults`. It deliberately publishes no
binary because a production image embeds credentials.

When authorized, run the independent live suite in
[Hardware testing](HARDWARE_TESTING.md), then the monitor in
[Soak testing](SOAK_TESTING.md). Do not run the negative HIL probes during an
existing soak. Save reports outside the repo;
they may contain site addressing and configuration even though they never
contain the bearer token.

## Private package

After a clean `idf.py reconfigure build`, run:

```sh
python3 tools/package_release.py
```

The packager verifies the ESP32-P4 descriptor and hash, project/version/flash
identity, embedded clean Git revision, OTA size, rollback, watchdog panic, and
core-dump policy. It then creates `release/private/vVERSION/` atomically with
mode 0700 and files mode 0600. The package includes app-only OTA and merged USB
recovery images, individual flash files, exact offsets, SHA-256 sums, a manifest,
the pinned public certificate/client, commissioning notes, and third-party
licenses. It never includes the plaintext token or standalone private key, but
the binaries contain both and remain secret.

The script refuses an existing version directory, symlinked output/input paths,
path traversal in ESP-IDF metadata, a dirty build revision, and output inside
any non-ignored repository directory. Copy the completed directory to an
approved secret store; do not attach it to a public GitHub release.

Increment `PROJECT_VER` for every distributable firmware behavior change. Keep
the ESP-IDF project name stable because the device rejects another project.

## Validation boundary

[VALIDATION_HISTORY.md](VALIDATION_HISTORY.md) records dated host, CI, HIL,
browser, OTA and endurance results, including the historical v1.19 watchdog
rollback test and later releases. Read its limitations before claiming a test
passed. New host-only/docs commits do not require a firmware flash: distinguish
repository HEAD from the last verified deployed source/image.

Still field-only:

- measure each real switch at the controller and electrically stimulate
  GPIO20/GPIO21/GPIO22 through the intended interface;
- verify cold boot, cable loss, PoE interruption/brownout, and rapid power-cycle
  behavior with recovery access present;
- complete a 24-hour soak;
- import/discover from each representative production BAS client;
- provision and validate Secure Boot/flash encryption only under a separate
  device-key lifecycle plan.
