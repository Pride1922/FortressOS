# Multiple-package laptop test

All generated disk images, including the [discarded fresh installer](pkg-fresh-installer-laptop.md), were removed at the user's request. These updater instructions are historical. A new image awaits separate instructions.

This candidate extends the accepted `snul1hju` kernel B installation. Keep the
existing account, home files and rot13 2.0. **Do not reinstall the baseline.**
Physical acceptance of this new candidate is pending.

Matching artifacts:

- Kernel-only USB: `bin/fortress-pkg-multiple-rfc4ecw0-update-usb.img`.
- New CLI: `build/kernel-update/pkg-multiple-rfc4ecw0/pkg-next.elf`.
- Hashes, exact predecessor binding and sources:
  `build/kernel-update/pkg-multiple-rfc4ecw0/manifest.json`.

Identify the removable USB on Bazzite as in
[kernel update instructions](kernel-update-bazzite.md). Flash this update image
using the existing whole-USB `dd` procedure, boot it, select the intended internal
disk, and confirm `UPDATE` after verification. The updater is bound to the exact
accepted kernel B; it refuses a different predecessor. It writes the inactive
kernel bank and selectors while preserving the root filesystem byte-for-byte.
Kernel B remains the previous selectable kernel. The new default kernel reports
`[KERNEL RELEASE] development`.

The updater remains kernel-only. The new CLI is a sidecar so the accepted base
`/bin/pkg` can stay in place during testing. Serve the directory containing
`pkg-next.elf` from another reachable machine, for example:

```sh
python3 -m http.server 8080 --bind 0.0.0.0 --directory build/kernel-update/pkg-multiple-rfc4ecw0
```

After booting the updated laptop, use that machine's address for `SERVER`.
Replace `/home/operator` with your own home directory if your login differs:

```sh
wget -O /home/operator/pkg-next http://SERVER:8080/pkg-next.elf
sha256sum /home/operator/pkg-next
```

Before executing it, verify the hash is:

```text
49f573b24c32dfb0b9002953e6bb54ca7b59aa39c0ad2282c6884c4185577fdc
```

Then:

```sh
chmod 755 /home/operator/pkg-next
/usr/bin/rot13 --version
sudo /home/operator/pkg-next installed
sudo /home/operator/pkg-next info rot13
sudo /home/operator/pkg-next upgrade
sudo /home/operator/pkg-next upgrade -y
```

Expected: rot13 remains 2.0 and its installed build remains 2. With the current
rot13-only catalog, an up-to-date invocation succeeds without prompting. To test
additional applications, publish their signed artifacts and a newer catalog
through the existing repository process, retaining predecessor objects. Then:

```sh
sudo /home/operator/pkg-next install TOOL
sudo /home/operator/pkg-next installed
sudo /home/operator/pkg-next upgrade TOOL
sudo /home/operator/pkg-next upgrade
```

There is no `update` or `get` prerequisite. The last command previews installed
packages with newer signed builds and asks before downloading artifacts. Use
`upgrade -y` for noninteractive operation. A failed package stops the batch and
prints completed, failed and unattempted outcomes; completed upgrades persist.
Reboot and check both commands, their installed builds and existing home files.
The inventory limit is 16 applications; ordinary remove remains unsupported.

The new normal build includes this CLI as `/bin/pkg`. Installing an authenticated
base-userland update is separate from this kernel-only/sidecar test. Keep using
the sidecar path on the existing laptop installation until that deployment is
explicitly performed.

USB image SHA-256:

```text
8f926e4a7bc2e723fc89fdf6563dbae0e9f503de8b71f72800d1a8869fd39878
```
