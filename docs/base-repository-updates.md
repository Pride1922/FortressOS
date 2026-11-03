# Base OS updates through the repository

[Implementation evidence and acceptance limits](roadmap/base-repository-updates.md).

The complete A/B installer is `bin/fortress-base-installer.img` (528 MiB).
SHA-256: `c90403dcf73940630b334cc0021882a5af553f8e47c25256c3783d287d0c2063`.
It supersedes the live-only diagnostic image and earlier installers, and provisions
complete base-update slots. Installation erases the selected target;
flashing/installing remain user-operated.

The image includes five [desktop application packages](desktop-packages.md):
Desktop, Files, Notepad, Terminal and Calculator, plus the base recovery compositor
and services. [Release evidence](roadmap/boot-timing-installer-2026-11-03.md)
and [Dell acceptance checklist](dell-acceptance-2026-11-03.md).

After installation, use the existing repository for applications and the base OS:

```sh
pkg base-status
sudo pkg install rot13
sudo pkg upgrade
# or, to accept the displayed upgrades without prompting:
sudo pkg upgrade -y
```

When the catalog offers a newer `fortress-base`, upgrade prepares it after the
application upgrades and reports that reboot is required. Reboot normally. The
boot confirmation service confirms the running set after the system reaches the
userspace supervisor. `pkg base-status` reports the result; explicit confirmation
is available with `sudo pkg confirm-base`. Another base update cannot overwrite
the previous set before the selected new boot is confirmed.

A repository must actually offer a signed newer base release for this to download
an update. Building an unsigned artifact does not publish it. Production signing is now configured; see the
[server deployment record](pkg-repository-ubuntu.md). The current catalog offers
applications; a newer base release must be built and published when available.

## Publisher workflow

Build the new kernel and normal bootstrap without creating another installer:

```sh
make -j4 bin/fortress.elf bin/initramfs.tar
objcopy --strip-debug bin/fortress.elf build/base-kernel.elf
python3 scripts/build_base_package.py \
  --kernel build/base-kernel.elf --bootstrap bin/initramfs.tar \
  --platform-id "$PLATFORM_ID" --build 2 \
  --predecessor-min 1 --predecessor-max 1 --base-abi 1 \
  --out build/fortress-base-2.fpk
```

Use the installed image's platform ID from its build receipt and the existing
repository signing identity. Increase the build for every changed base artifact.
The example is the first upgrade from baseline build 1; later releases must use
their actual increasing build and supported predecessor range. Layout 1 accepts
only the same configuration/user ABI: an ABI-changing or configuration-migrating
release is refused rather than overwriting settings or making rollback unsafe.

Publish with the existing tool and retain other catalog entries:

```sh
python3 scripts/pkg_repository.py \
  --repository 00010203-0405-0607-0809-0a0b0c0d0e0f --root-version 1 \
  --revision "$CATALOG_REVISION" --issued "$ISSUED" --expires "$EXPIRES" \
  --key "$SIGNING_SEED" --retain-existing \
  --artifact build/fortress-base-2.fpk --output "$REPOSITORY_DIR"
```

The signing seed remains outside the checkout and HTTP document root, with the
existing restrictive permissions. Catalog revisions increase globally across app
and base publication. Objects are durably published before the catalog. Serve the
same `catalog.v1` and `objects/sha256/<hash>.fpk` layout from the existing HTTPS
repository. See [Ubuntu publication](pkg-repository-ubuntu.md).

## Preservation and recovery

Each boot set contains its own kernel, bootstrap, readiness and release record.
The selected bootstrap supplies `/bin`, `/sbin`, `/lib`, `/base`, `/docs`, base
wallpapers/licenses/timezone data, and `/etc/ssl` certificate assets. The immutable
ownership map is fixed by layout version; artifacts cannot supply arbitrary write
paths or scripts.

Accounts, SSH host keys, desktop/network/timezone settings, home directories,
installed applications and package records stay on ext4. Bootstrap configuration
defaults are not copied over existing `/etc` files during an update. Base releases
that introduce a command owned by an installed app are rejected.

The previous complete set remains available in the Limine menu. If a new boot
fails, select the previous installed base or its authenticated recovery entry.
Automatic failed-boot detection/fallback is not implemented by the static menu.
The base update path does not replace the bootloader or alter partition geometry.

Inspect failures before retrying:

```sh
pkg base-status
sudo pkg recover-base
```

Recovery makes menu copies agree on the verified running set and can reset an
incomplete inactive release record. It does not lower the independently retained
repository/build floors. Interrupted preparation can then be retried using the
same authenticated artifact or a newer compatible release. An uncertain I/O
result fences updating in that boot; reboot before recovery. Recovery itself
runs from a verified installed base, uses retained local boot data and needs no
network or unexpired catalog. The authenticated recovery entry keeps its existing
read-only recovery role; it does not grant a mounted-root raw-write lease.

## Format and implementation limits

`FORTBAS1` has a 256-byte little-endian header: version/header bytes at 8/12,
release build at 16, predecessor minimum/maximum at 24/32, kernel/bootstrap lengths
at 40/48, platform at 56, payload SHA-256 values at 88/120, layout/ABI at 152/160,
and zero reserved bytes at 168..255. Kernel then unpersonalized USTAR bootstrap
follow the header. The bootstrap contains a 64-byte `base/release.v1` identity.
The whole object remains within the existing catalog v1 artifact-size ceiling;
this implementation does not silently increase application FPK limits.

The ESP reserves two 8 MiB kernels, two 8 MiB bootstraps, two 512-byte readiness
records, two release records and two alternating floor records. Runtime never
changes FAT chains, directory entries, GPT, BIOS code or root filesystem blocks.
Inactive payloads are flushed/read back before readiness, release publication and
finally the four menu selectors. Confirmation tears are recognized only when the
alternate READY/CONFIRMED checksum validates; they retain the unconfirmed state.

The installed-root adapter serializes each bounded ESP I/O with the same ext4
exclusion that protects ordinary NVMe/root operations and shutdown. Allocation,
verification and owner checks happen outside that lock. It does not relax the
installer raw-lease exclusion of mounted/source devices or expose disk/LBA choices
to userspace.

Host verification and firmware/physical acceptance are separate. The current task
uses focused host sanitizer/namespace/installer checks and normal builds. No QEMU
campaign or physical-device writes are part of this delivery.
