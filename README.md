# ESP32-P4 PoE BACnet toggle inputs

Standalone ESP-IDF firmware for the Waveshare **ESP32-P4-POE-ETH**. It reads
maintained-contact switches on GPIO20, GPIO21, and GPIO22; exposes them as
read-only BACnet/IP Binary Input objects; publishes device-health diagnostics;
supports ReadPropertyMultiple and COV; and accepts authenticated HTTPS
application updates over Ethernet after the initial USB installation.
The target definition matches the board's 32 MB onboard NOR flash.

The firmware uses DHCP. Its BACnet identity does not depend on its IP address:
Device instance **599152**, UDP port **47808** (`0xBAC0`). The device has most
recently used `192.168.75.152`, but a DHCP reservation is recommended if its
management address must remain stable.

## Wiring

Each input is configured input-only with an internal pull-down. Open is
`Inactive`; connecting the GPIO to the board's 3.3 V rail through a dry-contact
switch is `Active`.

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

ESP 3V3 ---- toggle switch ----+---- GPIO22
                               |
                              10 kΩ
                               |
ESP GND -----------------------+
```

Important:

- Use the header labels **GPIO20**, **GPIO21**, and **GPIO22**, not connector
  position numbers 20, 21, and 22.
- Use only the board's **3.3 V** rail. Never apply 5 V, 12 V, or 24 V.
- Power the board off while wiring.
- A switch from 3.3 V to an input is not a supply short. The GPIO must remain
  input-only, as enforced by this firmware. Do not configure it as an output.
- The 10 kΩ pull-downs are strongly recommended. Internal pulls are weaker and
  more susceptible to noise.
- For long cables, outdoor wiring, wiring near motors/mains, or building-control
  signals, use an isolated/conditioned dry-contact interface.

Relevant P1 header positions from the board schematic:

| Signal | P1 position |
|---|---:|
| GPIO22 | 32 |
| GPIO21 | 34 |
| GPIO20 | 35 |
| ESP 3V3 | 36 |

GPIO20 remains present to preserve existing BACnet links. GPIO22 is the
additional field-test alternative if this particular board's GPIO20 behavior
is affected by external circuitry. The firmware reserves the Waveshare
Ethernet connections and refuses to use those pins as inputs: MDC GPIO31, MDIO
GPIO52, PHY reset GPIO51, RMII data/control GPIO28/29/30/34/35/49, and RMII
clock GPIO50. See the [Waveshare schematic](https://files.waveshare.com/wiki/ESP32-P4-ETH/ESP32-P4-ETH-datasheet.pdf).

## BACnet objects and services

| Object | Instance | Default name | Meaning |
|---|---:|---|---|
| Device | 599152 | ESP32-P4 Toggle Inputs | Device identity |
| Binary Input | 20 | GPIO20 Toggle | Debounced GPIO20 state |
| Binary Input | 21 | GPIO21 Toggle | Debounced GPIO21 state |
| Binary Input | 22 | GPIO22 Toggle | Debounced GPIO22 state |
| Analog Value | 1000 | Chip Temperature | ESP32-P4 die temperature, °C |
| Analog Value | 1001 | System Uptime | Current boot uptime, seconds |
| Analog Value | 1002 | Free Heap | Current free heap, bytes |
| Analog Value | 1003 | Minimum Free Heap | Lowest free heap since boot, bytes |
| Analog Value | 1004 | Ethernet Link Losses | Link-down count since boot |
| Analog Value | 1005 | Ethernet Reconnects | Reconnect count since boot |
| Analog Value | 1006 | BACnet RX Packets | Accepted datagrams since boot |
| Analog Value | 1007 | BACnet Protocol Errors | Malformed/error transaction count |
| Analog Value | 1008 | Last Reset Reason | Numeric ESP-IDF reset reason |
| Analog Value | 1009 | Active COV Subscriptions | Current subscription count |

Supported services:

- Who-Is / I-Am, with unicast replies for direct requests and broadcast replies
  for subnet/global discovery, including the routed form used by Metasys
- confirmed ReadProperty
- confirmed ReadPropertyMultiple, including All/Required/Optional selectors
- confirmed SubscribeCOV for the three Binary Inputs
- confirmed and unconfirmed COV notifications

COV supports eight simultaneous subscriptions, lifetimes up to seven days,
initial notification, value/reliability change notifications, confirmed
acknowledgements, and bounded retries. Confirmed retries preserve the original
notification bytes until acknowledged. The server remains deliberately
read-only and rejects WriteProperty. It is not a BBMD, foreign device, BACnet
router, or BACnet/SC node; discovery requires the local subnet or an existing
BACnet/IP router.

The input debounce default is 50 ms. Per-input polarity can be reversed in
`menuconfig`. The electrical test classifies each line as `floating-open`,
`externally-high`, `externally-low`, or `unstable`. The first three are valid
line conditions; only an unstable/invalid test sets BACnet `Reliability` to
`unreliable-other` and the `fault` bit in `Status_Flags`.

## Build and initial flash

Use ESP-IDF **5.5.4**. The defaults target ESP32-P4 revision 0.x/1.x, matching
this Waveshare board; do not use the binary on revision 3.x silicon.

Private OTA material is never tracked. For a new device, generate its
certificate/key/token before building:

```sh
python3 tools/generate_ota_credentials.py --force
```

This writes the public certificate to `main/ota_server_cert.pem` and writes the
private key and bearer token to ignored, permission-restricted files under
`secrets/`. Back up `secrets/` in an approved secret store. A later clone cannot
manage the device without that token and key. To build with a securely supplied
directory instead, set `ESP32_P4_OTA_SECRETS_DIR`; it must contain
`ota_server_key.pem` and `ota_token.txt`. Every OTA-enabled build validates the
token, certificate lifetime and server-auth purpose, P-256 PKCS#8 key format,
private file permissions, and certificate/key match before compiling.

Perform the first installation over USB. The project uses a dual-slot
partition table, so erase an older single-image layout once:

```sh
idf.py set-target esp32p4
idf.py build
idf.py -p PORT erase-flash flash monitor
```

Replace `PORT` with the local serial port. Exit the monitor with `Ctrl-]`.
Bootloader or partition-table changes always require USB; OTA updates only the
application slot.

Use `idf.py menuconfig` to change factory defaults. After first boot, BACnet
identity, object names/instances, input polarity, debounce time, and BACnet UDP
port are stored in NVS and managed through the authenticated `/config` API.
GPIO assignments remain fixed at build time so a configuration mistake cannot
claim an Ethernet or power-related board pin. DHCP hostname and HTTPS port also
remain build-time settings for now. Verify any source-level pin change against
the board schematic.

## Authenticated Ethernet management

The HTTPS server provides five bearer-authenticated operations:

- `GET /ota/status` — firmware/partition/rollback state, Git revision, reset
  reason, exact running-image SHA-256, uptime, temperature, heap, watchdog
  health, Ethernet negotiation, DHCP/address state, BACnet counters, input
  pad/raw/debounced/transition data, self-test results, and the persistent fault
  log.
- `POST /diagnostics/input-self-test` — weak-pull line classification. It does
  not enable GPIO output drivers and may be run with field wiring connected.
- `GET /config` — export the complete saved BACnet/input configuration and show
  whether it differs from the active configuration.
- `PUT /config` — validate and atomically persist a complete configuration.
  Changes take effect only after restart, preventing a partial live change from
  disrupting an active BACnet session.
- `POST /ota` — validated application-image upload and reboot.

DHCP's public ESP-IDF API does not expose lease expiry. Status therefore reports
DHCP state, address/netmask/gateway, acquisition count/time, address age, and IP
change count; `address_age_ms` is not lease time remaining.

Check status or classify the connected input lines:

```sh
python3 tools/ota_client.py status --host 192.168.75.152
python3 tools/ota_client.py input-self-test --host 192.168.75.152
python3 tools/ota_client.py config-get \
  --host 192.168.75.152 --output device-config.json
```

To commission the BACnet identity or inputs, save the `config-get` JSON object
to a file, edit it, and send the complete object back. Read-only result fields
such as `database_revision`, `active_database_revision`, and
`restart_required` may remain in the file and are ignored on input:

```sh
python3 tools/ota_client.py config-put --host 192.168.75.152 device-config.json
```

The accepted response reports `restart_required: true`. A normal OTA update or
the later authenticated reboot command activates it. Sending the active values
again cancels a pending configuration. Each effective change increments the
BACnet Device `Database_Revision`; the configured Device and Binary Input
instances and object names must be unique and within BACnet limits.

Build and upload an application:

```sh
idf.py build
python3 tools/ota_client.py upload \
  --host 192.168.75.152 \
  build/esp32_p4_bacnet_switches.bin
```

The client pins the exact certificate in `main/ota_server_cert.pem` before it
sends any HTTP data. CA and hostname checking are replaced by exact DER
certificate pinning because the device uses a self-signed leaf certificate and
DHCP. The server additionally requires a 32–128
character bearer token, validates its embedded certificate/key pair and running
image at startup, verifies each uploaded ESP image and project name, writes only
an inactive OTA slot, and uses bootloader rollback. Uploads are blocked while a
new image is pending so the known-good rollback slot cannot be overwritten.
Health validation begins after ten seconds; the image must establish Ethernet,
DHCP, HTTPS OTA, BACnet, and healthy monitored tasks for five consecutive
samples within 60 seconds or it is marked invalid and rolled back automatically.

Before uploading, the client validates the ESP segment layout, ROM checksum,
appended SHA-256, 32 MB flash header, chip target, and project name. After
acceptance it requires the device to echo the exact accepted-image hash, waits
through reboot, and reports success only when that same image is running in the
`valid` state.
Use `--no-wait` only when another system will perform that verification. For a
credential rotation, supply `--post-cert` and `--post-token-file` so the same
command verifies the replacement credentials after reboot.

### Credential rotation and security boundary

To rotate remotely, first preserve the currently deployed certificate/token in
a secure temporary location. Generate replacement credentials, build the new
application, upload it using the old client credentials, then verify status
using the replacements. Confirm the former certificate/token are rejected
before destroying the temporary copy.

The private key and token are embedded in each device image, so generated
application or merged binaries are also secrets and must not be committed or
attached to a public release. Secure Boot and flash encryption are not enabled
because eFuse provisioning is irreversible and device-specific. Someone with
physical flash access can therefore recover credentials unless those controls
are provisioned separately.

### Persistent diagnostics

The NVS-backed 16-entry fault log survives reboot and records boot/reset,
abnormal reset (panic, brownout, watchdog, power glitch, or CPU lockup), link/IP
loss, OTA accepted/failed/validated, input-test failures, temperature setup
failure, BACnet socket failure, HTTPS server startup failure, and task-watchdog
registration failure. Entries include sequence, boot count, boot-relative time,
type, and error code.

The switch-input and BACnet tasks are registered with ESP-IDF's task watchdog.
The status endpoint reports their registration, most recent heartbeat, and
health. Firmware version and the source Git revision are independently exposed
through HTTPS and BACnet `Application_Software_Version`.

All millisecond uptime, heartbeat, IP-age, transition, and fault-log timestamps
are 64-bit JSON integers, so they do not wrap after 49.7 days.

### Vendor ID warning

The default Vendor Identifier **999** and vendor name **Lab placeholder** are
for private lab use only. Before distribution as a product, replace both with
the organization's assigned BACnet Vendor ID and matching name.

## Host-side tests

The host suite compiles with warnings-as-errors plus AddressSanitizer and
UndefinedBehaviorSanitizer. It covers reference discovery vectors,
ReadProperty, ReadPropertyMultiple, COV, error/reject/abort paths, bounded
output buffers, malformed/truncated/random frames, constant-time bearer
authentication, and secure credential tooling:

```sh
cmake -S tests -B build-tests
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

The full target check is:

```sh
idf.py reconfigure
idf.py build
```
