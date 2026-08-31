# ESP32-P4 PoE BACnet toggle inputs

Standalone ESP-IDF firmware for the Waveshare **ESP32-P4-POE-ETH**. It reads
two maintained-contact toggle switches on GPIO20 and GPIO21, publishes them as
read-only BACnet/IP Binary Input objects, and accepts authenticated HTTPS
application updates over Ethernet after the initial USB installation.

The firmware uses DHCP. Its BACnet identity is independent of its changing IP
address: Device instance **599152**, UDP port **47808** (`0xBAC0`). Device
instance 599152 was not present when `192.168.75.0/24` was surveyed, but it must
remain unique across the full BACnet internetwork.

## Wiring

The proposed 3.3 V wiring works, provided each GPIO is pulled low while its
switch is open. This firmware enables each ESP32-P4 internal pull-down and
interprets open as `Inactive` and closed as `Active`.

For reliable permanent wiring, install one **10 kΩ external pull-down resistor**
per input:

```text
ESP 3V3 ---- toggle switch ----+---- GPIO20
                               |
                              10 kΩ
                               |
ESP GND -----------------------+

ESP 3V3 ---- toggle switch ----+---- GPIO21
                               |
                              10 kΩ
                               |
ESP GND -----------------------+
```

Important:

- Use the header labels **GPIO20** and **GPIO21**, not physical connector pin
  positions 20 and 21.
- Use only the board's **3.3 V** rail. Never apply 5 V, 12 V, or 24 V to these
  GPIOs.
- Power the board off while wiring.
- The 10 kΩ resistors are strongly recommended. Internal pull-downs are weaker
  and more vulnerable to noise.
- For long cables, outdoor wiring, wiring near motors/mains, or existing
  building-control signals, do not connect directly to the GPIO. Use an
  isolated/conditioned dry-contact input circuit instead.

GPIO20 and GPIO21 are available on this board and do not overlap its Ethernet
management pins. The firmware follows the board-specific IP101GRI connections:
MDC GPIO31, MDIO GPIO52, PHY reset GPIO51, PHY address 1, RMII clock input.
These values come from the
[Waveshare schematic](https://files.waveshare.com/wiki/ESP32-P4-ETH/ESP32-P4-ETH-datasheet.pdf)
and [official ESP-IDF Ethernet example](https://github.com/waveshareteam/ESP32-P4-Platform/tree/main/examples/esp-idf/11_ethernetbasic).

## BACnet objects

| Object | Instance | Default name | Present_Value |
|---|---:|---|---|
| Device | 599152 | ESP32-P4 Toggle Inputs | n/a |
| Binary Input | 20 | GPIO20 Toggle | `Inactive` / `Active` |
| Binary Input | 21 | GPIO21 Toggle | `Inactive` / `Active` |

Supported network services are Who-Is/I-Am and confirmed ReadProperty. An I-Am
is broadcast after DHCP succeeds and is also returned for matching Who-Is
requests. The switch values are debounced for 50 ms. BACnet clients poll the
Binary Input `Present_Value` property.

The BACnet implementation is intentionally read-only. It rejects WriteProperty
and does not implement outputs, COV, ReadPropertyMultiple, BBMD/foreign-device
registration, routing, or BACnet/SC. Discovery therefore stays on the local IP
subnet unless a BACnet/IP router forwards it.

## Build and flash

ESP-IDF 5.4 is the closest match to Waveshare's current board examples. IDF 6.x
is also accounted for through the conditional IP101 managed-component
dependency.

From an ESP-IDF terminal on the flashing computer, perform the first
OTA-capable installation over USB. This firmware introduces a dual-slot
partition table, so erasing the old flash layout is recommended for this one
installation:

```sh
cd esp32-p4-bacnet-switches
idf.py set-target esp32p4
idf.py build
idf.py -p PORT erase-flash flash monitor
```

Replace `PORT` with the serial port, such as `COM5` on Windows or
`/dev/cu.usbmodem...` on macOS. Exit the monitor with `Ctrl-]`.

Prebuilt release files are also available under `release/`: a merged image for
the first USB installation and an application-only image for later OTA use.
See `release/README.md` for commands and SHA-256 checksums.

To change BACnet names, object instances, GPIOs, debounce time, or DHCP
hostname:

```sh
idf.py menuconfig
```

Open **BACnet toggle input configuration**, save, then rebuild and flash.
If changing the GPIO defaults, verify the new pins against the board schematic;
the firmware refuses known Ethernet-reserved GPIOs.
OTA port and bearer token settings are under **Authenticated HTTPS OTA
configuration**.

## Authenticated Ethernet updates

After the first USB flash, future **application** images can be installed over
Ethernet. The device exposes only these authenticated HTTPS endpoints:

- `GET /ota/status` — running project, version, partition, and rollback state.
- `POST /ota` — raw ESP-IDF application image upload.

The server uses a project-specific ECDSA certificate, a 64-character bearer
token, a 4 MiB inactive OTA slot, ESP image validation, project-name checking,
and ESP-IDF bootloader rollback. A newly uploaded application is marked valid
only after running for 10 seconds without crashing.

Build the next application and check the device:

```sh
idf.py build
python3 tools/ota_client.py status --host 192.168.75.152
```

Update `PROJECT_VER` in the top-level `CMakeLists.txt` when assigning a new
release version; the status endpoint and serial log report that value.

Upload `build/esp32_p4_bacnet_switches.bin`:

```sh
python3 tools/ota_client.py upload \
  --host 192.168.75.152 \
  build/esp32_p4_bacnet_switches.bin
```

The client reads `provisioning/ota-token.txt`, verifies the TLS chain, then
compares the peer certificate with the exact pinned certificate in
`main/ota_server_cert.pem`. Hostname checking is intentionally replaced by
exact certificate pinning because the device uses DHCP. A successful upload
returns HTTP 202 and reboots the device.

Use the device's current DHCP address if it is no longer `192.168.75.152`.
Only the application image can be updated this way. Bootloader or partition
table changes still require USB flashing.

### OTA credential handling

This source package contains the initial device certificate private key and a
unique initial bearer token. Treat the project and ZIP as secrets. The initial
certificate has SHA-256 fingerprint
`460050a29ae68d4c85bd7709f18584696315620b700010c3df814e85bda9fb36`
and expires on **2036-08-28**.

Before production deployment, generate a new certificate/key and bearer token.
Update `CONFIG_OTA_BEARER_TOKEN`, `provisioning/ota-token.txt`, and the embedded
PEM files together. To rotate credentials remotely, upload the newly built app
using the old credentials, then switch the client to the new token/certificate
after reboot.

HTTPS plus the bearer token protects network updates. Secure Boot and flash
encryption are deliberately not enabled because provisioning their eFuses is an
irreversible, device-specific operation. Someone with physical flash access can
therefore recover embedded credentials unless those protections are separately
provisioned.

### Vendor ID warning

The default BACnet Vendor Identifier **999** and vendor name **Lab placeholder**
are for private lab use only. Before distributing or deploying this as a
product, replace both values with the organization's assigned BACnet Vendor ID
and matching vendor name.

## Verify

The serial log should show link up, the DHCP address, and an I-Am announcement.
A BACnet browser on the same subnet should discover Device 599152. Read these
properties while moving each switch:

```text
binary-input,20  Present_Value
binary-input,21  Present_Value
```

The project currently uses DHCP rather than hard-coding `192.168.75.152`; that
was the device's observed address before this firmware was installed and may be
reassigned. Use a DHCP reservation if a stable management IP is desired.

## Host-side protocol tests

The packet codec and constant-time OTA authentication helper are isolated from
ESP-IDF and have strict AddressSanitizer and UndefinedBehaviorSanitizer tests,
including known BACnet reference vectors, truncated frames, bounded output
buffers, malformed/random input, and valid/invalid authorization headers:

```sh
cmake -S tests -B build-tests
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```
