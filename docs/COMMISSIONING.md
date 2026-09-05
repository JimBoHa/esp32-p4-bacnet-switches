# Commissioning and recovery

## 1. Confirm the hardware

Use only the Waveshare `ESP32-P4-POE-ETH` with the 32 MB ESP32-P4 revision 0.x/
1.x family targeted by this project. Do not flash firmware from the S3
8DI/8RO-C controller; its processor, PHY, I/O expanders, partitions, and pin map
are different.

Inputs are P1 GPIO20 (position 35), GPIO21 (34), and GPIO22 (32). P1 position 36
is 3.3 V. Never apply 5/12/24 V. A dry contact from 3.3 V to an input is valid,
with one 10 kΩ pull-down from that input to ESP GND strongly recommended. For a
100-foot field loop or wiring near power equipment, use an isolated,
surge-protected dry-contact interface rather than depending on the ESP's weak
internal pull. Eighteen-AWG copper resistance is not enough by itself to explain
a 3.3 V signal collapsing at the controller; measure both ends under load and
check the chosen header positions, common ground, contact continuity, and PoE
board documentation.

## 2. Verify the private release

The release directory is secret because its binaries embed device credentials.
Verify it before flashing:

```sh
shasum -a 256 -c SHA256SUMS
```

Use `sha256sum -c SHA256SUMS` on Linux. Confirm `manifest.json` names
`Waveshare ESP32-P4-POE-ETH`, chip `esp32p4`, 32 MB flash, the intended source
revision, and the expected application hashes.

## 3. Initial USB recovery flash

Identify the exact serial port. If automatic download mode fails, hold BOOT,
tap RESET, then release BOOT. Erasing removes firmware, NVS configuration, and
OTA state:

```sh
python -m esptool --chip esp32p4 --port /dev/DEVICE erase-flash
python -m esptool --chip esp32p4 --port /dev/DEVICE \
  --baud 460800 write-flash 0x0 initial-flash.bin
```

Never guess the serial path. On Windows it is normally a `COM` port. Keep the
USB recovery image and the matching certificate/token backup available until
commissioning and rollback tests finish.

## 4. Discover and authenticate

Default networking is DHCP. Find the lease by serial log, switch/DHCP lease
table, BACnet Who-Is, or local mDNS name `esp32-p4-bacnet.local`. The current
lab reservation is `192.168.75.152`; do not assume it at another site.

```sh
python3 management/ota_client.py status \
  --host DEVICE_IP \
  --cert management/ota_server_cert.pem \
  --token-file /secure/path/ota_token.txt
```

Confirm project, version, source revision, exact image hash, `valid` state,
Ethernet/IP, watchdog health, and the reported security/recovery posture.

## 5. Configure identity and network

Export the complete BACnet configuration, edit it, PUT it, reboot, and verify.
Device instances and all object names/instances must remain unique. Vendor ID
999 and `Lab placeholder` are private-lab defaults, not a distributable vendor
identity.

```sh
python3 management/ota_client.py config-get \
  --host DEVICE_IP --output device-config.json
python3 management/ota_client.py config-put \
  --host DEVICE_IP device-config.json
```

Use `network-get`/`network-put` for DHCP or static IPv4. A changed address runs
as a 60-second trial and must be confirmed through the new address with
`network-confirm`; otherwise the previous confirmed settings return
automatically. Reserve the address and check for conflicts first.

## 6. Check inputs and BACnet

If the wiring position is uncertain, open the no-login diagnostics dashboard's
**P1 header: all 40 positions** table first. Physical position is not a GPIO
number: for example, P1-22 is GPIO47, while GPIO22 is P1-32. Consult the
[complete header map and reserved-pin limitations](HEADER_DIAGNOSTICS.md).
Operate one switch and compare repeated readings on the matching GPIO; a HIGH
alone does not establish that a floating or shared-bus pin is a driven switch.
Only GPIO20/21/22 are configured BACnet switch channels. Additional header
readings do not add BACnet objects or change that mapping.

With field wires disconnected, GPIO20/GPIO21/GPIO22 must all show raw and stable
false and BACnet `Inactive`. Run `input-self-test`; an open line should classify
`floating-open`. `externally-high` and `externally-low` are valid driven states;
`unstable` is a fault. The test uses only weak internal pulls and never enables
an output driver.

Then connect one protected switch at a time. Measure voltage at the controller
GPIO-to-GND, operate the switch, and verify raw, debounced, and BACnet values.
Send directed and broadcast Who-Is, scan the complete 27-object list, read all
properties, and verify COV. This firmware has no physical outputs to actuate.

## 7. Ethernet OTA

```sh
python3 management/ota_client.py upload \
  --host DEVICE_IP \
  --cert management/ota_server_cert.pem \
  --token-file /secure/path/ota_token.txt \
  firmware-ota.bin
```

Success means the client observed the accepted inactive slot reboot into the
exact project/version/hash and reach `valid`. Do not use `--no-wait` in normal
commissioning. Use `--allow-same-image` only to intentionally restore a damaged
redundant slot.

## Recovery

- **No IP:** inspect link LEDs, PoE power, VLAN, DHCP leases, serial logs, and
  the guarded static-address trial. Use USB recovery if needed.
- **All inputs unexpectedly low:** measure the 3.3 V header-to-GND supply, then
  each GPIO-to-GND at the controller; verify header position and common ground.
- **Unstable input:** disconnect field wiring and run self-test. If open is
  stable, diagnose the cable/contact/interface. If still unstable, inspect the
  board/PoE circuitry and move the field input to GPIO22 as already modeled.
- **Lost/compromised token:** USB recovery or a credential-rotation OTA using
  still-valid old credentials. There is no network backdoor.
- **Rejected/crashing OTA:** preserve power and the known-good slot. Status will
  show `failed` or `rolled-back`; collect the persistent fault log before erase.
- **Bootloader/partition change:** application OTA cannot install it; use USB.
