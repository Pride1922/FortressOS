# Bazzite USB: rot13 1.0 installer and upgrade

The candidate is `bin/fortress-rot13-1.0-installer.img`, a normal flashable
installer with `rot13` 1.0 already installed in its root template. It uses the
normal account/password setup and existing HTTPS repository/trust configuration.
The older `build/pkg-upgrade-qemu/.../fortress-rot13-1.0.img` is a separate QEMU
fixture and is not this installer.

SHA-256: `17a45575fdf65c8c46fe1c612955bd61693555b8ddf246484652839563f5046f`.
The build/runtime receipt is `bin/fortress-rot13-1.0-installer.manifest.json`;
its result must be `PASS` before using the candidate.

October 31 acceptance: **PASS**, BIOS and UEFI USB installation, followed by
SMP=4 live HTTPS upgrade/no-op and reboot. Evidence:
`build/rot13-installer/accept-m6vbo3it/manifest.json`.

## Flash from Bazzite

The user's October 31 inventory identifies `/dev/sda` as the SanDisk USB disk,
with `/dev/sda3` mounted at `/run/media/fabio/FORTRESSLOG`. `/dev/nvme0n1` is the
internal Bazzite system disk. Check the identification again after reconnecting
devices; flashing erases the entire selected USB disk.

```sh
lsblk -o NAME,SIZE,MODEL,TRAN,MOUNTPOINTS
sha256sum "$HOME/Projects/FortressOS/bin/fortress-rot13-1.0-installer.img"
udisksctl unmount -b /dev/sda3
sudo dd if="$HOME/Projects/FortressOS/bin/fortress-rot13-1.0-installer.img" \
  of=/dev/sda bs=4M status=progress conv=fsync
sync
udisksctl power-off -b /dev/sda
```

If a partition is already unmounted, omit its unmount command. Unmount any other
mounted `/dev/sda` partitions before flashing. Use the whole device `/dev/sda`,
not a partition such as `/dev/sda1`.

## Install and upgrade

Boot the USB on the Dell through F12 and choose **FortressOS Installer**. Select
the intended FortressOS target disk and complete normal account/recovery setup.
The installer erases the disk selected in that screen. After success, remove
the USB and boot the installed system. No package-install command is needed:

```sh
/usr/bin/rot13 --version
echo hello | /usr/bin/rot13
```

Expect `rot13 1.0` and `uryyb`. With network connectivity and the correct clock:

```sh
pkg update
pkg get rot13
sudo pkg upgrade rot13
/usr/bin/rot13 --version
echo hello | /usr/bin/rot13
sudo pkg upgrade rot13
```

Expect `upgrade complete`, `rot13 2.0`, `uryyb`, and then
`package is up to date rot13 v 2.0`. After a normal reboot, verify version 2.0
and filter output again. `/usr/bin` is not in the current default login PATH.

## Build and verify

Inside the existing `fortress-dev` Ubuntu container, with the normal build tools
and Python cryptography installed:

```sh
cd /home/fabio/Projects/FortressOS
make -j4 bin/fortress.elf bin/initramfs.tar build/tool-install-multi.elf limine-setup
python3 scripts/create_rot13_installer.py
make test-rot13-installer
```

The producer refuses to overwrite an existing candidate. It downloads and
hash-checks the retained published 1.0 object, runs the actual FPK validators,
and seeds the template using the actual package backend, EXT4 and JBD2 under
ASan/UBSan. A private host source copy sets the adapter's fixed clock and skips
the seed's deliberately free descriptor-growth padding during allocation. The
unchanged actual installer expansion is checked on a copy before packaging.
The existing public demo signing identity authenticates the seed catalog;
no private signing material or host adapters enter the media. The original
template receipt is retained before updating payload hashes for the seeded root.

The acceptance runner installs copies of the actual candidate USB onto fresh
regular-file NVMe disks, using the normal installer without a disposable-kernel
write bypass. It personalizes credentials, checks the exact 1.0 ELF, boots under
SMP=4, upgrades from the real HTTPS repository, checks a verified no-op and
reboots. Linux oracles check filesystem integrity, ELF bytes, installed-record
checksum/build and preserved account/repository files. The runner uses KVM for
BIOS and UEFI. It never opens a physical disk or changes the shipped image.
