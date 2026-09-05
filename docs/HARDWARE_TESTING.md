# Hardware acceptance testing

The HIL runner is non-actuating. It validates independent BACnet discovery,
the ordered 18-object model and metadata, protocol capability bits,
ReadPropertyMultiple `ALL`, Who-Has/I-Have, confirmed/unconfirmed COV, the
eight-subscription capacity boundary, error paths, mDNS, pinned HTTPS,
authentication rejection, configuration-name protections, runtime diagnostics,
and exact firmware identity.

```sh
python3 -m pip install -r requirements-hil.txt
PYTHONPATH=/path/to/pinned/site-packages python3 tools/bacnet_hil_test.py \
  --local-address HOST_IP/24 \
  --device-address DEVICE_IP \
  --device-instance 599152 \
  --mdns-hostname esp32-p4-bacnet \
  --expect-inputs-off \
  --expected-version VERSION \
  --expected-source GIT_REVISION \
  --expected-image-sha256 IMAGE_SHA256 \
  --token-file /secure/path/ota_token.txt \
  --report /secure/path/hardware-report.json
```

The local BACnet client binds UDP 47808; stop another BACnet process using that
host address first. The suite temporarily creates and cleans up COV
subscriptions but does not drive GPIO, change persistent configuration, reboot,
or update firmware.

Run the authenticated line classifier with switches/wires disconnected, then
with one field circuit at a time. It cannot measure volts or source current. A
metered electrical test must confirm controller-side 3.3 V/high and 0 V/low,
contact continuity, input polarity, and real debounce behavior.

Version 1.19.0's controlled negative OTA test used a separately built image
whose switch task intentionally stopped servicing its watchdog. The task
watchdog reset it, ESP-IDF marked the candidate `aborted`, the bootloader
returned to the exact known-good image, and the persistent log reported
`ota-rolled-back`. The inactive slot was then overwritten with the healthy
image and revalidated. Do not repeat destructive rejection or fault-injection
tests without stable power, a private recovery package, and physical USB access.

After finite acceptance, run the [soak monitor](SOAK_TESTING.md).
