# ESP32-P4 firmware 1.1.0

## First USB installation

`esp32_p4_bacnet_switches-1.1.0-full.bin` contains the bootloader, dual-slot
partition table, initial OTA metadata, and application. Flash it at offset
`0x0` after erasing the device:

```sh
python -m esptool --chip esp32p4 erase_flash
python -m esptool --chip esp32p4 write_flash \
  0x0 esp32_p4_bacnet_switches-1.1.0-full.bin
```

The equivalent ESP-IDF workflow is documented in the project README.

## Future Ethernet installation

`esp32_p4_bacnet_switches-1.1.0-ota.bin` is an application-only image suitable
for the authenticated HTTPS OTA endpoint:

```sh
python3 ../tools/ota_client.py upload \
  --host DEVICE_IP \
  esp32_p4_bacnet_switches-1.1.0-ota.bin
```

Do not flash the OTA-only image at offset `0x0`.

## SHA-256

```text
c96b0dfd5f808782cec81ea0c5008277f1c7feb3bcb8f425f0ffe8fbc37cad71  esp32_p4_bacnet_switches-1.1.0-full.bin
4bf798a3cf2f6c987b73e6bb485c0884dfa3c4354a7a2cc0581fc3f996648f52  esp32_p4_bacnet_switches-1.1.0-ota.bin
```

This package contains the corresponding OTA bearer token and private TLS key.
Keep the complete package private.
