# Private firmware artifacts

Do not commit firmware binaries from this project. Every OTA-enabled binary
contains that device's bearer token and TLS private key.

After generating or securely supplying credentials, build a private
application-only OTA image with:

```sh
idf.py build
cp build/esp32_p4_bacnet_switches.bin \
  release/esp32_p4_bacnet_switches-1.3.0-ota.bin
shasum -a 256 release/esp32_p4_bacnet_switches-1.3.0-ota.bin
```

Upload it with:

```sh
python3 tools/ota_client.py upload \
  --host DEVICE_IP \
  release/esp32_p4_bacnet_switches-1.3.0-ota.bin
```

For a private merged image used during the initial USB installation:

```sh
cd build
esptool.py --chip esp32p4 merge_bin -o \
  ../release/esp32_p4_bacnet_switches-1.3.0-full.bin \
  @flash_args
```

Keep binaries and checksums in an approved secret store. They are ignored by
Git. The project README contains the direct `idf.py` USB procedure.
