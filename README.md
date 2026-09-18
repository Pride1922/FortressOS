# FortressOS

**A 64-bit operating system built from the kernel up.**

FortressOS is a freestanding C11 and x86-64 assembly kernel with isolated user
processes, an interactive shell, and a writable ext2 filesystem. It boots through
[Limine](https://github.com/limine-bootloader/limine) under UEFI or legacy BIOS,
runs in QEMU, and has been tested on a real Dell Latitude 5590.

It is an independent kernel, not a Linux distribution. The project is still
under active development: its emphasis is on explicit ownership, bounded failure
paths, and tests that distinguish emulator results from hardware observations.

**Current milestone:** bootable ISO and raw disk image packaging are implemented
(Phase 9F). **Next:** kernel USB storage access and persistence on the boot stick
(Phase 9G).

## What works today

| Area | Implemented capabilities |
| --- | --- |
| Memory | Physical page allocator, four-level paging, per-process address spaces, guarded thread stacks, and a kernel heap. Physical allocation currently covers the low 2 GiB. |
| CPU and scheduling | GDT/IDT, exception diagnostics, dedicated double-fault/NMI stacks, ACPI discovery, APIC timer, and preemptive scheduling. Current synchronization assumes a single CPU. |
| User programs | Ring 3 execution, ELF loading, System V AMD64 argument passing, validated syscalls, child waiting, exit status, and deferred process reclamation. |
| Storage | PCI discovery, NVMe reads/writes/flush, validated GPT partitions, and bounded read/write ext2 support. Persistence is verified on disposable QEMU NVMe fixtures. |
| Files | Read, create, write, truncate, make directories, rename/move, and delete. Initramfs provides the boot-time programs and files. |
| Interaction | Framebuffer text console, PS/2 keyboard, US and Belgian AZERTY layouts, serial input, a Ring 3 shell, and a small text editor. |
| Boot and power | BIOS/UEFI boot images, boot splash, ACPI shutdown, and reset fallbacks. Shutdown and reboot have been manually verified on the Dell. |

User-process fault isolation and resource reclamation have targeted tests; this
is not a claim of complete security isolation. ext2 writes support direct and
single-indirect blocks, with explicit rejection of unsupported structures.
The filesystem does not promise crash-atomic updates or recovery from arbitrary
power loss.

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

The current QEMU launch targets also attach a separate NVMe test disk. Its ext2
partition supplies `/mnt`; booting the raw image as USB does not establish that
the kernel can access that USB device.

To try saving files on the disposable QEMU NVMe fixture:

```bash
make run-bios WRITE_TEST=1
```

This explicitly enables writable mounting of the fixture. Use `shutdown` or
`poweroff` in the shell to flush the filesystem and finish a clean session.

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

With the writable QEMU fixture mounted:

```text
mkdir /mnt/notes
edit /mnt/notes/hello.txt
cat /mnt/notes/hello.txt
mv /mnt/notes/hello.txt /mnt/notes/saved.txt
shutdown
```

The editor has its own `help` command. Shell commands also include `rm`,
`reboot`, and conditional chaining with `&&` and `||`.

## Real hardware: what has been verified

Manual testing on a **Dell Latitude 5590** (Core i5-8350U, 32 GiB installed RAM,
256 GB NVMe, Intel UHD 620) has confirmed USB boot, the framebuffer console,
PS/2 keyboard input, Belgian AZERTY behavior, shell interaction, argument
passing to `/bin/hello`, shutdown, and reboot.

**USB boot works; USB storage inside FortressOS does not yet.** Firmware and
Limine load the kernel and initramfs from the stick. Once running, the kernel
has no USB controller or mass-storage driver with which to read its data
partition. Flashing `fortress.img` with Rufus therefore does not yet provide
persistent `/mnt` storage on the Dell.

The internal physical NVMe is deliberately excluded from the QEMU storage
test/mount path. Adding an ext2 partition to the laptop's SSD does not enable
automatic mounting. The physical allocator also still uses only the low 2 GiB
of RAM, regardless of how much is installed.

## Next: a USB stick that can keep your files

Phase 9G connects the existing image, block-device API, and ext2 implementation
through a new USB storage path:

1. **xHCI bring-up:** PCI discovery, MMIO/reset, command completion, port
   inspection, and validated device enumeration in separate checkpoints.
   PCI-only discovery (9G.1a) is verified in BIOS/UEFI QEMU and confirmed in a
   Dell boot photo; controller initialization remains pending.
2. **Read-only USB storage:** Bulk-Only Transport and a bounded set of SCSI
   commands, exposed through the existing block-device interface.
3. **A real USB `/mnt`:** explicit partition selection and a read-only mount,
   tested without an NVMe fixture that could hide missing USB support.
4. **Writes and persistence:** device flush, explicit writable opt-in, clean
   shutdown, and repeated save/reboot/read verification.

The initial scope is directly attached USB 2.0 mass storage present at boot,
using LUN 0. SuperSpeed, external hubs, hot-plug/reconnection, UAS, and other USB
device classes are deferred. Physical Dell acceptance is separate from QEMU
acceptance. Accounts, permissions, and an installer are later milestones.

See [ROADMAP.md](ROADMAP.md) for checkpoint history and the staged plan, and
[AGENTS.md](AGENTS.md) for implementation contracts and acceptance criteria.

## Testing

The project combines host sanitizer tests, QEMU integration tests, offline
filesystem checks, and manual hardware observations.

| Command | Coverage |
| --- | --- |
| `make test-input` | Keyboard decoding, modifiers, and bounded input FIFO |
| `make test-usb-discovery` | PCI-only xHCI detection/absence and shell startup in BIOS/UEFI, without an NVMe fixture |
| `make test-console` | Framebuffer rendering, wrapping, scrolling, and bounds |
| `make test-ext2` | Actual ext2/VFS code under ASan/UBSan, malformed data and injected failures |
| `make test-ext2-write` | BIOS/UEFI multi-boot persistence on disposable NVMe images, with offline `e2fsck -fn` |
| `make test-storage` | BIOS/UEFI GPT/ext2, user-space reads, and allocation audits |
| `make test-shell` | Shell interaction, input wakeups, process execution, and reclamation |
| `make test-nmi` | NMI injection at exact syscall transition boundaries in QEMU |
| `make test-boot-diagnostics` | UEFI boot with 8 GiB RAM and no COM1 |
| `make test-power` | QEMU shutdown and reboot commands |

Recorded test results and their limits live in the roadmap. A listed test target
is not a claim that every revision has passed it, and QEMU success is not
physical-hardware acceptance. Storage tests must use disposable images, never
an existing physical disk.

## Finding your way around

```text
src/arch/x86_64/   CPU setup, interrupts, context switching, syscall entry
src/kernel/       Boot, scheduler, processes, ELF loader, syscalls
src/mm/           Physical memory, paging, heap
src/drivers/      Console, input, PCI, NVMe, power
src/fs/           VFS, tar initramfs, GPT, ext2
user/             Freestanding user programs and shell
tests/            Host tests and mocks
scripts/          Image creation and QEMU verification
```

Before changing kernel code, read [AGENTS.md](AGENTS.md). For supported formats,
architectural limits, and technical debt, see [ARCH_REVIEW.md](ARCH_REVIEW.md).
Contributions should state what changed, which tests were run, and whether the
evidence comes from host tests, QEMU, or physical hardware.
