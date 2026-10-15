FortressOS
A 64-bit operating system built from the kernel up.

FortressOS is a freestanding C11 and x86-64 assembly kernel with isolated user processes, an interactive shell, journaled ext4, a from-scratch TCP/IP stack, and Unix-style permissions. It boots through Limine under UEFI or BIOS, runs in QEMU, and has been tested on real Dell Latitude hardware.

It is an independent kernel, not a Linux distribution.

What works today
Boot — UEFI and BIOS, quiet graphical splash, clean handoff to Ring 3.
Multi-core — 8 cores, per-CPU state, work-stealing scheduler, IPIs, TLB shootdown. Race-hardened, verified on hardware.
Memory — 32 GiB physical allocator, paged virtual memory, per-process address spaces, deferred reclamation. Correctness-gated and profiled.
Filesystems — journaled ext4 (default, JBD2 crash recovery) and ext2 (supported for legacy fixtures and the live USB data partition).
Storage — NVMe, USB mass storage (xHCI, BOT/SCSI), per-device durability classification.
Networking — Intel e1000/e1000e/I219, Ethernet, ARP, IPv4, ICMP, UDP, TCP, userspace DNS. Bare-metal throughput matches Linux Mint on identical hardware.
Permissions — Unix DAC, UID/GID, mode bits, login, sudo, setuid, capabilities. Hardware-verified.
Userland — Ring 3 processes, modular shell with job control and pipelines, nano v2.1, and a full stream-tool suite.

Design principles
Explicit ownership. Every resource has a defined owner and lifetime.
Bounded failure paths. No unbounded retries, no silent degradation.
Evidence over claims. Tests distinguish host, QEMU, and physical hardware results.
Every behavior is documented with its evidence boundary. Nothing claims more than it has proven.

Try it in QEMU
Build on Linux, or WSL Ubuntu-24.04 on Windows.

sh
sudo apt-get install -y build-essential nasm xorriso qemu-system-x86 ovmf \
    git curl e2fsprogs python3 gdb mtools dosfstools

git clone https://github.com/Pride1922/FortressOS.git
cd FortressOS
make
Produces bin/fortress.elf, bin/initramfs.tar, bin/fortress.iso, and bin/fortress.img.

sh
make run-bios       # ISO, legacy BIOS
make run-img        # Raw image, UEFI
make run-img-usb    # Raw image presented as USB storage
make debug          # Paused for GDB on port 1234
Boot from USB
sh
sudo dd if=bin/fortress.img of=/dev/sdX bs=4M status=progress conv=fdatasync
The image carries a FAT32 EFI partition (Limine, kernel, initramfs) and a 64 MiB ext2 data partition (read-only by default; writable via usb_data_mode=rw). The normal image's primary filesystem is journaled ext4.

Verified on Dell Latitude 5590 and 5530 with multiple USB devices.

Documentation
AGENTS.md — implementation contracts and invariants (authoritative)
PROTECTED.md — protected subsystems and contracts
COMMANDS.md — every shell builtin, tool, and shortcut
docs/roadmap/ — checkpoint history and hardware evidence
docs/subsystems/ — subsystem references
docs/plans/ — architectural plans

ARCH_REVIEW.md — limits and technical debt

Status
Complete and hardware-verified: boot, SMP, memory, ext2/ext4, shell, networking, permissions, VFS lifetime, TCP ISN.

In progress: persistent root / NVMe installer (Phase 4).

Downstream: package manager, SSH, cross-core sockets, userland threads.

FortressOS is under active development. See AGENTS.md for the current state; this README is a human overview and may lag by a commit or two.