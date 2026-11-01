# Kernel and package update laptop test

These matching images passed BIOS and UEFI QEMU kernel/package/recovery acceptance.
The user confirmed the laptop baseline installation and kernel update PASS on
2026-11-01, including reboot and preserved account/home-file/rot13 1.0 checks.
The physical package-upgrade and previous/recovery boot checks remain pending.
Use them in order:

| Purpose | Image |
| --- | --- |
| One-time baseline installation, kernel A and rot13 1.0 | `bin/fortress-kernel-baseline-snul1hju.img` |
| Offline kernel update, preserving the installed root | `bin/fortress-kernel-update-usb-snul1hju.img` |

The baseline installer replaces the selected laptop disk. The update USB performs
no installation, partition change or root-filesystem write. The same physical USB
can be flashed with the baseline first, then reflashed with the updater after the
baseline is installed and boot-tested. Do not install the update USB as an OS.

On Bazzite, identify the removable USB with:

```sh
lsblk -o NAME,SIZE,MODEL,TRAN,MOUNTPOINTS
```

Unmount its mounted partitions, then write the chosen `.img` to the **whole USB
device**, using the same `dd ... bs=4M status=progress conv=fsync` procedure as the
previous installer. The existing Bazzite NVMe is not a USB target. No physical
drive was flashed by the image producer or acceptance tests.

Boot the baseline installer, choose the laptop's intended internal disk and finish
account/password setup. Remove the USB and boot the installed OS. Check:

```sh
/usr/bin/rot13 --version
dmesg | grep 'KERNEL RELEASE'
```

Expected: `rot13 1.0` and `kernel-update-baseline-1`. Create a small file in your
  home directory to check preservation later. Shut down cleanly.

Flash the matching update USB image onto the USB and boot it. Select the internal
disk by its displayed model/serial. It checks the exact predecessor kernel, GPT,
FAT allocation graph, both boot slots, root UUID binding and four menu copies.
After `Verified installation`, type `UPDATE`. Wait for `KERNEL UPDATE COMPLETE`,
remove the USB and reboot from the internal disk.

Expected: `kernel-update-candidate-2`, the same account and home file, and still
`rot13 1.0`. These are two test builds of the same user ABI; the release marker
proves which kernel executed. The menu retains kernel A installed/recovery and
kernel B installed/recovery. Kernel B is now the default; kernel A remains available.

Then test the independent package update while connected to the network:

```sh
pkg update
pkg get rot13
sudo pkg upgrade rot13
/usr/bin/rot13 --version
```

Expected: `rot13 2.0`. Reboot again and repeat the version/home-file checks. Finally,
select kernel A's installed entry once: it should also see the persisted rot13 2.0
and the same account/files.

This first updater is kernel-only, uses physically trusted USB media and is bound
to this exact baseline. It does not update the common bootstrap or base userland,
and has no automatic failed-boot rollback. Reusing it after a successful update
is a read-only version refusal. An I/O failure or mixed menu-selection state stops
without a mutation retry; retain the USB and diagnostic output for investigation.

Image SHA-256:

```text
d13cfe1d88c82d168fc49addf7bc066c195232f9f2fc8bdd566dc5c2cb583270  fortress-kernel-baseline-snul1hju.img
f682616674a410a034b4323df5a850e623e0afa106dea7fdd17f2321abc63afe  fortress-kernel-update-usb-snul1hju.img
```

Acceptance and exact source/image hashes: `bin/fortress-kernel-update.manifest.json`.
