# Complete installer — 2026-11-02

**Superseded and removed.** Use the [complete A/B base-update installer](base-repository-updates.md).
The image below is historical and did not provision base repository-update slots.

Image: `bin/fortress-ready-2026-11-02-installer.img` (528 MiB).
Source checkpoint: `98f9cf0bcf317ec814d6d66b073c65b7aedcd22f`.

Includes the complete current installed system: latest kernel, multiple-package
backend and automatic-fetch CLI, Wi-Fi enhancements, Network & Wi-Fi desktop
redesign, base applications, services, Intel 8265 firmware, and existing HTTPS
repository configuration and public trust key. The installation package inventory
starts empty. Installer media uses a minimal installer-only bootstrap; the root
payload contains the complete system.

SHA-256:

```text
f1eb40d0dfa82d2feb7da18b6546f75cf1685bcbb6a7160090b168cec23ef7e0
```

Build receipt: `bin/fortress-ready-2026-11-02-installer.manifest.json`.
The producer completed GPT/FAT/payload integrity checks. No QEMU campaign or
physical boot was performed. No physical device was written. Duplicate transport
images were removed after publication to conserve disk space.

Flash the complete image using the existing whole-device USB procedure. Boot the
installer, select the intended disk by model/serial, and complete account setup.
Installation erases the selected target. Remove USB and boot the installed system.

Application installation and updates use `sudo pkg install NAME`,
`sudo pkg upgrade`, and `sudo pkg upgrade -y`, without a separate catalog-fetch
step. Repository: `https://fortress.ameyerocha.com`.

The delivery goal is subsequent updates through that repository. Application
updates are implemented. Desktop publication/acceptance and repository-based
kernel/bootstrap/base-userland updates remain separate work; this image does not
implement the latter. The existing offline kernel updater is predecessor-bound.
