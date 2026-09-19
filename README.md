# FortressOS

**A 64-bit operating system built from the kernel up.**

FortressOS is a freestanding C11 and x86-64 assembly kernel with isolated user processes, an interactive shell, and a writable ext2 filesystem. It boots through [Limine](https://github.com/limine-bootloader/limine) under UEFI or legacy BIOS, runs in QEMU, and has been tested on a real Dell Latitude 5590.

It is an independent kernel, not a Linux distribution. The project is still under active development: its emphasis is on explicit ownership, bounded failure paths, and tests that distinguish emulator results from hardware observations.

**Current milestone:** USB storage and real `/mnt` persistence are implemented (Phase 9G.4). The kernel now enumerates an xHCI controller, addresses a USB mass-storage device over Bulk-Only Transport, registers it as a block device, parses its GPT, and mounts its ext2 partition read-write at `/mnt`. Files written from the shell persist across a full power cycle on the Dell. **Next:** USB topology expansion — multiple xHCI controllers, SuperSpeed enumeration, and hubs (Phase 9G.5).

## What works today

| Area | Implemented capabilities |
| --- | --- |
| Memory | Physical page allocator, four-level paging, per-process address spaces, guarded thread stacks, and a kernel heap. Physical allocation currently covers the low 2 GiB. |
| CPU and scheduling | GDT/IDT, exception diagnostics, dedicated double-fault/NMI stacks, ACPI discovery, APIC timer, and preemptive scheduling. Current synchronization assumes a single CPU. |
| User programs | Ring 3 execution, ELF loading, System V AMD64 argument passing, validated syscalls, child waiting, exit status, and deferred process reclamation. |
| Storage | PCI discovery, NVMe reads/writes/flush, xHCI + USB Mass Storage BOT, validated GPT partitions, and bounded read/write ext2 support. Persistence verified on both QEMU NVMe fixtures and a physical USB 2.0 stick. |
| USB | xHCI controller bring-up, device enumeration and descriptor parsing, BOT/SCSI reads and writes, durability classification with per-device policy, and explicit writable opt-in. USB 2.0 direct-attached devices only. |
| Files | Read, create, write, truncate, make directories, rename/move, and delete. Initramfs provides boot-time programs; ext2 provides persistent storage. |
| Interaction | Framebuffer text console, PS/2 keyboard, US and Belgian AZERTY layouts, serial input, a Ring 3 shell, and a small text editor. |
| Boot and power | BIOS/UEFI boot images, boot splash, ACPI shutdown, and reset fallbacks. Shutdown and reboot have been manually verified on the Dell. |

User-process fault isolation and resource reclamation have targeted tests; this is not a claim of complete security isolation. ext2 writes support direct and single-indirect blocks, with explicit rejection of unsupported structures. The filesystem does not promise crash-atomic updates or recovery from arbitrary power loss.

USB durability is classified per device and disclosed in the boot log. A device that reports its caching page and accepts `SYNCHRONIZE CACHE` gets the strong guarantee; a device that reports neither is mounted write-through on the assumption that it behaves like every other consumer stick, with an explicit warning that power loss during writes may lose data. Physical power-loss tolerance is not claimed for any device.

## Try it in QEMU

Build on Linux, or use WSL **Ubuntu-24.04** on Windows. Install the tools:

```bash
sudo apt-get update
sudo apt-get install -y build-essential nasm xorriso qemu-system-x86 ovmf \
    git curl e2fsprogs python3 gdb mtools dosfstools

git clone https://github.com/Pride1922/FortressOS.git
cd FortressOS
make
make run
```

`make` fetches missing Limine dependencies and produces:

| Artifact | Purpose |
| --- | --- |
| `bin/fortress.elf` | Kernel ELF |
| `bin/initramfs.tar` | Boot-time files and user programs |
| `bin/fortress.iso` | Bootable ISO |
| `bin/fortress.img` | 130 MiB raw GPT disk image with a 64 MiB FAT32 EFI partition and a 64 MiB ext2 data partition |

Useful launch targets:

```bash
make run-bios       # ISO, legacy BIOS
make run-img        # Raw image, UEFI when firmware is available
make run-img-bios   # Raw image, legacy BIOS
make run-img-usb    # Raw image presented as USB storage, UEFI when available
make debug         # Start paused for GDB on port 1234
```

The `run-img*` targets attach a separate NVMe test disk whose ext2 partition supplies `/mnt` — that is the QEMU storage fixture, not the USB path. To exercise the real USB code path in QEMU, use the USB-specific runners below.

### Booting the raw image from a USB stick

`bin/fortress.img` can be flashed to a USB stick and booted on real hardware:

```bash
sudo dd if=bin/fortress.img of=/dev/sdX bs=4M status=progress conv=fdatasync
```

The image carries two partitions: a FAT32 EFI System Partition with Limine, the kernel, and the initramfs, and a 64 MiB ext2 data partition. The default boot entry mounts the data partition read-only. A second boot entry opts into a writable mount of the same partition, matched by `PARTUUID`.

On the Dell Latitude 5590, the Kingston USB DISK 2.0 (VID `0x13FE`, PID `0x4200`) enumerates as a USB 2.0 mass-storage device. Its ext2 partition mounts read-write; files written from the shell persist across a full power cycle; `e2fsck -fn` on the unmounted stick reports 0 errors.

To try saving files on the disposable QEMU NVMe fixture (not the USB path):

```bash
make run-bios WRITE_TEST=1
```

Use `shutdown` or `poweroff` in the shell to flush the filesystem and finish a clean session.

## At the shell

```text
help
ls /
ls /bin
cat /etc/motd
layout azerty
run /bin/hello hello FortressOS
echo $?
```

With the USB stick's data partition mounted read-write:

```text
mkdir /mnt/notes
edit /mnt/notes/hello.txt
cat /mnt/notes/hello.txt
mv /mnt/notes/hello.txt /mnt/notes/saved.txt
sync
shutdown
```

After reboot, `cat /mnt/notes/saved.txt` returns the same file.

The editor has its own `help` command. Shell commands also include `rm`, `sync`, `reboot`, and conditional chaining with `&&` and `||`.

## Real hardware: what has been verified

Manual testing on a **Dell Latitude 5590** (Core i5-8350U, 32 GiB installed RAM, 256 GB NVMe, Intel UHD 620) has confirmed USB boot, the framebuffer console, PS/2 keyboard input, Belgian AZERTY behavior, shell interaction, argument passing to `/bin/hello`, ACPI shutdown, and reboot.

USB storage is verified on physical hardware. The kernel discovers the Dell's xHCI controller at `0000:00:14.0`, addresses a Kingston USB stick, parses its GPT, mounts its ext2 partition read-write, and persists files written from the editor across a full power cycle. `e2fsck -fn` on the unmounted stick from Linux reports 0 errors.

The internal physical NVMe is deliberately excluded from the USB storage mount path by parent-device provenance. Adding an ext2 partition to the laptop's SSD does not enable automatic mounting. The physical allocator still uses only the low 2 GiB of RAM, regardless of how much is installed.

**What is not verified on hardware:** the strong durability paths. A SanDisk USB 3.x stick was tested and correctly identified as a SuperSpeed device on Port 0x12, then skipped by the current scope — USB 3.x support is Phase 9G.5b. The strong durability classification (`WRITE_THROUGH`, `SYNC_BACKED`) is exercised in QEMU only; on physical hardware the tested stick falls into `ASSUMED_WRITE_THROUGH` because it does not report its cache policy.

## USB storage and durability

The USB path is a self-contained driver set under `src/drivers/xhci*` and `src/fs/usb_mount.c`. It targets xHCI controllers only; EHCI/UHCI/OHCI are out of scope.

**Supported today:**
- Directly attached USB 2.0 Full-Speed and High-Speed mass-storage devices.
- BOT transport with LUN 0 only.
- Read, write, and `SYNCHRONIZE CACHE` where the device supports them.
- Partition selection by `PARTUUID` with USB parent-device provenance.
- Explicit `usb_data_mode=rw` opt-in; the default boot entry is read-only.

**Not supported:**
- USB 3.x SuperSpeed (rejected with a clear log message; Phase 9G.5b objective).
- External hubs (class 0x09; rejected).
- Hot-plug or reconnection.
- UAS, or any device class other than mass storage.

**Durability classification** is per-device and printed in the boot log:

- `SYNC_BACKED` — the device accepts `SYNCHRONIZE CACHE`; every flush must complete.
- `WRITE_THROUGH` — `MODE SENSE` reports `WCE=0`; the device reports no write cache.
- `ASSUMED_WRITE_THROUGH` — the device reports neither, but is mounted RW with an explicit disclosure that power-loss during writes may lose data.
- `READ_ONLY` — the device reports a write cache it cannot flush, or is in an error state.

The classification is what gates writable mount: only devices that can either prove their durability or accept the fallback disclosure get RW. A device that reports `WCE=1` and rejects `SYNCHRONIZE CACHE` is refused.

## Next: USB topology expansion

Phase 9G.5 extends the USB stack to more machines and more devices:

1. **Multiple xHCI controllers.** Many laptops and most desktops present more than one controller. The current driver stops at the first; the current single-controller path leaves devices on the second unreachable.
2. **SuperSpeed enumeration.** USB 3.x port link state, slot and endpoint context layout, and SuperSpeed descriptor handling. Required to reach the SanDisk-class devices that report a caching page.
3. **Strong durability on real hardware.** Once a USB 3.x device enumerates, its cache policy can be read and the strong classification exercised on physical hardware.
4. **Persistence on a second device class.** Closes the "verified on two independent devices" claim.
5. **Hubs (deferred).** Until a hub-attached device is required.

Physical Dell acceptance is separate from QEMU acceptance. Accounts, permissions, and an installer are later milestones.

See [ROADMAP.md](ROADMAP.md) for checkpoint history and the staged plan, and [AGENTS.md](AGENTS.md) for implementation contracts and acceptance criteria.

## Testing

The project combines host sanitizer tests, QEMU integration tests, offline filesystem checks, and manual hardware observations.

| Command | Coverage |
| --- | --- |
| `make test-input` | Keyboard decoding, modifiers, and bounded input FIFO |
| `make test-usb-discovery` | PCI-only xHCI detection/absence and shell startup in BIOS/UEFI, without an NVMe fixture |
| `make test-usb-descriptors` | Device addressing, descriptor parsing, BOT class validation, and `SET_CONFIGURATION` in QEMU BIOS/UEFI |
| `make test-usb-block` | BOT transport, SCSI reads, and GPT registration in QEMU BIOS/UEFI, no NVMe fixture |
| `make test-usb-mount` | PARTUUID selection and read-only `/mnt` mount in QEMU BIOS/UEFI |
| `make test-usb-persistence` | QEMU BIOS/UEFI three-boot create/read/overwrite with offline `e2fsck -fn` on disposable 130 MiB images |
| `make test-xhci-bot-host` | BOT stall recovery, MODE SENSE parsing, and durability policy under ASan/UBSan |
| `make test-usb-mount-host` | Mount eligibility, durability modes, and sync path under ASan/UBSan |
| `make test-console` | Framebuffer rendering, wrapping, scrolling, and bounds |
| `make test-ext2` | Actual ext2/VFS code under ASan/UBSan, malformed data and injected failures |
| `make test-ext2-write` | BIOS/UEFI multi-boot persistence on disposable NVMe images, with offline `e2fsck -fn` |
| `make test-storage` | BIOS/UEFI GPT/ext2, user-space reads, and allocation audits |
| `make test-shell` | Shell interaction, input wakeups, process execution, and reclamation |
| `make test-nmi` | NMI injection at exact syscall transition boundaries in QEMU |
| `make test-boot-diagnostics` | UEFI boot with 8 GiB RAM and no COM1 |
| `make test-power` | QEMU shutdown and reboot commands |

Recorded test results and their limits live in the roadmap. A listed test target is not a claim that every revision has passed it, and QEMU success is not physical-hardware acceptance. Storage tests must use disposable images, never an existing physical disk.

## Finding your way around

```text
src/arch/x86_64/   CPU setup, interrupts, context switching, syscall entry
src/kernel/       Boot, scheduler, processes, ELF loader, syscalls
src/mm/           Physical memory, paging, heap
src/drivers/      Console, input, PCI, NVMe, xHCI, USB BOT, power
src/fs/           VFS, tar initramfs, GPT, ext2, USB mount policy
user/             Freestanding user programs and shell
tests/            Host tests and mocks
scripts/          Image creation and QEMU verification
```

Before changing kernel code, read [AGENTS.md](AGENTS.md). For supported formats, architectural limits, and technical debt, see [ARCH_REVIEW.md](ARCH_REVIEW.md). Contributions should state what changed, which tests were run, and whether the evidence comes from host tests, QEMU, or physical hardware.