# Private firmware artifacts

Every OTA-enabled binary embeds that device's admin/viewer tokens and TLS private key.
Never commit or attach one to a public release.

After a clean production build, create a verified private package:

```sh
idf.py reconfigure build
python3 tools/package_release.py
```

Output goes to ignored `release/private/vVERSION/`. The directory is mode 0700;
files are mode 0600. It contains an OTA image, merged USB recovery image,
individual flash inputs/offsets, manifest, checksums, pinned public certificate,
client, commissioning/security notes, and third-party licenses. It deliberately
omits the plaintext bearer-token and standalone private-key files, but the
binaries remain secret because both are embedded.

The packager refuses to overwrite a version, package a dirty or mismatched
build, follow symlinked paths, or write within a non-ignored repository path.
Move the complete package to an approved secret store after verification. See
[`docs/DEVELOPMENT.md`](../docs/DEVELOPMENT.md) and
[`docs/COMMISSIONING.md`](../docs/COMMISSIONING.md).
