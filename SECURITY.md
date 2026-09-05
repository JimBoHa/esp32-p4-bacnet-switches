# Security policy and operating model

## Reporting a vulnerability

Use [GitHub's private vulnerability-reporting form](https://github.com/JimBoHa/esp32-p4-bacnet-switches/security/advisories/new).
Do not open a public issue containing an OTA bearer token, TLS private key,
device firmware binary, site configuration, or BAS network inventory. Include
only the minimum reproduction data needed. The current `main` branch is the
only version receiving security fixes.

## Network boundary

BACnet/IP is intentionally read-only in this firmware, but BACnet/IP itself is
unauthenticated. Any host reaching UDP 47808 can read values, create bounded COV
subscriptions, and consume protocol resources. Put the controller on a
building-automation VLAN; allow BACnet only from required peers and allow TCP
443 only from management hosts. Do not expose either service to the Internet.
The firmware is not a BACnet/SC node, BBMD, or Foreign Device.

mDNS advertises only public identity and service metadata on the local link.
It never advertises credentials.

## Management and OTA

Every device-data HTTPS route requires a 32–128 character bearer token. Only the
static dashboard HTML/CSS/JavaScript shell is public; it contains no device data.
Distinct viewer and admin tokens are required at build time and startup.
Viewer tokens can read status, diagnostics reports, BACnet configuration, and
network configuration. All six mutation routes (OTA, configuration changes,
network confirmation, input self-test, and reboot) require admin, returning 403
for viewer credentials and 401 for absent/invalid credentials. The dashboard
remains read-only with either token and displays the authenticated role.

The client pins the
exact self-signed leaf certificate, requires TLS 1.2 or newer, and bounds device
responses. Certificate pinning replaces CA/hostname verification because the
device may use DHCP. It authenticates the expected device certificate; it does
not make a copied bearer token safe.

The server validates the certificate/key pair at startup and OTA uploads by
authentication, media type, size, whole-transfer deadline, ESP image structure,
project identity, RSA-PSS-3072/SHA-256 signature, secure version, inactive-slot
destination, and boot selection. ESP-IDF software verification uses the first
signing key from the current signed app. The client independently verifies the
signature against `main/ota_signing_public_key.pem` before uploading and confirms
the enforcing policy and key digest after reboot. Unsigned or wrong-key OTA
images cannot be installed through an enforcing device's OTA API.
A candidate remains pending until Ethernet, IP, HTTPS, BACnet, and both watched
tasks are healthy for five consecutive samples. Failure or a task-watchdog panic
reboots and lets the bootloader return to the known-good slot.

Keep `secrets/ota_server_key.pem`, `secrets/ota_token.txt` (admin), and
`secrets/ota_viewer_token.txt` in an approved secret store. Token files must be
mode 0600. Never pass a token on a command
line, in chat, an issue, a CI secret, or a commit. The public certificate in
`main/ota_server_cert.pem` is intentionally tracked and is not secret.

The independent `secrets/firmware_signing_key.pem` (RSA-3072, mode 0600) must stay
host-only. Never embed it, pass its contents on a command line, or place it in CI.
Keep a separate approved/offline backup. Losing it requires physical recovery;
this single-key workflow does not support remote key rotation. A stolen signing
key is firmware-authoring authority; HTTPS admin authentication is still required.
Serial recovery remains available. Permission-restricted backups are not encrypted.

OTA-enabled binaries contain the TLS private key and both bearer tokens. Treat every
application, merged recovery, ELF, core dump, and release bundle as secret.
CI signs with a disposable test key, never the production signing key. It
builds only the OTA-disabled configuration and publishes no
firmware artifact. `tools/package_release.py` writes only to a mode-0700 private
directory and creates mode-0600 files.

## Physical-device boundary

Secure Boot, flash encryption, and eFuse application anti-rollback are not
enabled. Someone with physical flash access can extract credentials or replace
firmware. Enabling those features is an irreversible, device-specific lifecycle
decision requiring separately backed-up signing/encryption keys and a proven
recovery procedure; do not enable them through an ordinary OTA update.

Flash core dumps are disabled because an unencrypted dump can retain credentials
or field data. Serial logs and authenticated status provide the supported fault
evidence. The status endpoint reports these limits explicitly.

## Credential exposure response

1. Revoke any exposed GitHub or infrastructure credential immediately.
2. Generate a new device certificate/key/token set in a secure temporary path.
3. Build and deploy with the old device credentials, then verify with the new
   certificate and token using `--post-cert` and `--post-token-file`.
4. Confirm the old token/certificate no longer work, then destroy temporary
   copies according to site policy.
5. If a secret entered Git history, rewrite all writable refs and contact GitHub
   Support to purge cached pull-request views and unreachable objects. Deleting
   a branch or force-pushing alone is not complete removal.
