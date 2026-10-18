# Combined Live USB / Installer image — 2026-10-18

The canonical `bin/fortress.img` is one 592 MiB raw image with exactly two
Limine menu entries: **FortressOS (Live USB)** and **FortressOS (Installer)**.
The former small image's installer menu entry was incomplete: it lacked the
installer executable/deployment modules. That packaging has been replaced.

`make img` reuses the existing immutable payload producer and installer media
builder. A private bootstrap adds `/bin/install` compiled with the existing
multi-disk UI. The installer entry loads bootstrap plus all five typed modules
(ESP, root, BIOS capsule, MBR code and hash manifest); the live entry loads only
bootstrap and selects its writable data partition by PARTUUID. No installer
payload roles are loaded in live mode. Base `bin/initramfs.tar` is not modified
to inject the installer. Combined packaging publishes no extra installer ISO.

| GPT partition | Size | Purpose |
| --- | --- | --- |
| 1 | 512 MiB | FAT32 boot volume, shared bootstrap and full installer payload |
| 2 | 1 MiB | Existing dedicated BIOS boot slot |
| 3 | 8 MiB | Existing fixed FAT16 installer log volume |
| 4 | 64 MiB | Journaled ext4 Live USB storage at `/mnt` |

Installer log binding accepts either the existing exact three-partition 528 MiB
layout or this exact four-partition 592 MiB layout. The new fourth partition
must have the expected Linux type, unique nonzero GUID, exact disjoint range
and zero attributes; extra entries reject. Both GPT copies, source disk identity
and immutable FAT log metadata still validate. The only permitted log writes
remain the same eight sectors (4096 bytes). Target disk selection, confirmation,
raw leases and installation/provisioning policies are unchanged.

The image is audited before publication: both GPT CRCs/arrays, nonoverlapping
ranges, the two exact menu labels, live PARTUUID, installer executable in the
archive, five module roles, payload lengths/hashes and independent Linux
`e2fsck -fn` on live storage. Temporary regular files are used throughout; image
construction does not write a physical USB or NVMe device. Small explicit legacy
fixture images remain live-only and do not advertise an incomplete installer.

## Verification

Commands for this delivery:

```sh
make -j4 bin/fortress.img
python3 scripts/test_default_ext4_image.py --firmware bios --cpus 8 --output build/combined-live-bios-acceptance
python3 scripts/test_default_ext4_image.py --firmware uefi --cpus 8 --output build/combined-live-uefi-acceptance
python3 scripts/test_install_log.py --media build/root-phase4/media-mbw4d25q --image-name fortress.img
python3 scripts/test_combined_installer.py
```

Both live persistence campaigns pass (BIOS/UEFI, SMP=8, two boots each): ordinary
operator create/sync/shutdown/reboot/read/unlink, Linux fsck after each boot,
journal feature verification, and unchanged shipped image. Evidence is in the
two named `build/combined-live-*-acceptance` directories.

Actual log-slot ASan/UBSan tests pass against a disposable copy of the combined
image, including source/GPT/FAT binding, error cases, readback and failure latches.
Independent byte comparison verifies every byte outside the fixed log file is
unchanged. Evidence: `build/root-phase4/logging-02o48ogd`.

The installer test selects menu entry 2 in a private image copy, attaches only
a disposable regular-file NVMe target, and checks that the full UI reaches real
disk selection and completes `INSTALL_DRY_RUN` over the actual payload. It stops
before account entry, confirmation and target acquisition; the target hash and
original image hash must remain unchanged. Limine default entries are one-based;
the harness's initial selection of entry 1 reached live login and was corrected
to entry 2. Both BIOS and UEFI pass: the full multi-disk installer reaches
selection, validates the actual deployment files, reports that the disk has not
been changed, and advances to account setup. The disposable target hash and
original image hash remain unchanged.

Final independent verification also passes:

```sh
python3 scripts/create_boot_img.py bin/fortress.img --verify
```

Delivered artifact: `bin/fortress.img`, 620,756,992 bytes (592 MiB). Packaging
manifest: `bin/fortress.manifest.json`; private producer evidence:
`build/root-phase4/media-mbw4d25q/manifest.json`.

Future combined regression: `make test-combined-image` (live persistence plus
installer dry-run). No full installation, installed-system reboot, new physical
Dell acceptance or new secret-bearing installed disk is claimed by these checks.
