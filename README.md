# FortressOS

<p align="center">
  <strong>A freestanding, 64-bit operating system built from the kernel up.</strong>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/architecture-x86__64-blue.svg" alt="Architecture: x86_64">
  <img src="https://img.shields.io/badge/bootloader-Limine%20v8%20(UEFI%2FBIOS)-brightgreen.svg" alt="Bootloader: Limine">
  <img src="https://img.shields.io/badge/language-C11%20%7C%20NASM-orange.svg" alt="Language: C11 / NASM">
  <img src="https://img.shields.io/badge/filesystem-Journaled%20ext4%20(JBD2)-informational.svg" alt="Filesystem: ext4">
  <img src="https://img.shields.io/badge/SMP-8%20Cores%20Verified-blueviolet.svg" alt="SMP: 8 Cores">
</p>

---

FortressOS is an independent, freestanding operating system kernel written in C11 and x86-64 assembly. It features preemptive multi-core scheduling, per-process virtual memory spaces, Unix-style discretionary access control and capabilities, a journaled ext4 filesystem, a custom TCP/IP networking stack, a modular userland shell, and a native text-mode installer.

FortressOS boots via Limine under UEFI or legacy BIOS, runs in QEMU, and is verified and physically accepted on bare-metal Dell Latitude hardware.

> [!NOTE]
> FortressOS is an independent kernel and operating system, not a Linux distribution or wrapper.

---

## What Works Today

### 🚀 Boot & Architecture
- **Firmware Support:** Native boot under both **UEFI** and **legacy BIOS** via Limine v8 (base revision 3).
- **Early Diagnostics:** Framebuffer console, graphical boot splash, and early COM1 serial output operational before heap initialization.
- **Ring 3 Transitions:** Clean user/kernel separation with atomic SYSRET/IRET transition frames.

### ⚡ Multi-Core (SMP) & Scheduling
- **Distributed Scheduler:** Work-stealing runqueues across up to 8 logical cores with load balancing.
- **Per-CPU State:** Independent CPU-local GS structures (`GS_BASE`), IST stacks, and TSS descriptors.
- **Inter-Processor Interrupts (IPIs):** Remote execution, reschedule signaling, and contention-safe TLB shootdown.
- **Verification:** Verified on QEMU (1, 4, 8 cores) and bare-metal Dell Latitude hardware (8 logical CPUs, 32 GiB RAM).

### 🧠 Memory Management
- **Physical Memory (PMM):** Frame allocator managing up to 32 GiB with contiguous region support and allocation set auditing.
- **Virtual Memory (VMM):** 4-level paging (PML4), higher-half kernel mapping (`0xffffffff80000000`), full HHDM coverage, and demand paging.
- **Address Space Lifetime:** Contention-safe TLB invalidation, per-process PML4 teardown, and deferred address-space reaping.

### 🗄️ Storage & Filesystems
- **Journaled ext4:** Default filesystem with native **JBD2 journaling**, crash recovery, extent trees, ordered writeback, and directory relocation (EH-6).
- **Filesystem Hardening:** Atomic file replacement (`SYS_RENAME_REPLACE` 84), 32-block staging write batches (`SYS_STAGE_WRITE` 85), and interactive SSH latency SLA guarantees.
- **Live Media Writable `/etc` (ETCFS):** In-memory mutable configuration tree with 16 MiB aggregate quota ceiling and copy-on-write TarFS backing for persistent live configurations.
- **NVMe Driver:** Direct PCI NVMe 1.4 storage controller engine with queue management and quiescent reset.
- **USB Mass Storage:** Native xHCI host controller driver with Bulk-Only Transport (BOT/SCSI) and durability classification.

### 🌐 Networking Stack
- **Custom Stack:** From-scratch implementations of Ethernet II, ARP, IPv4, ICMP, UDP, and TCP (with RFC 1071 checksums and Reno congestion control).
- **DNS & Network Configuration:** Native stub resolver, `/bin/nslookup` with UDP-to-TCP fallback, RFC 2131 DHCP client (`/sbin/dhcpc`), and consolidated configuration in `/etc/network/network.conf` and `/etc/network/resolv.conf`.
- **Hardware Drivers:** Intel e1000, e1000e, and physical Intel I219-LM (autonomous PHY renegotiation, cold-plug and flap recovery).
- **Cross-Core Networking:** Application calls from any CPU route through the CPU-0 protocol owner. Phases 0–4 are complete through bounded Dell physical acceptance; throughput redesign remains deferred. See [network SMP evidence](docs/roadmap/network-smp-phase4.md).

### 🔒 TLS / HTTPS Engine
- **BearSSL Platform Integration:** BearSSL 0.6 client profile under Gate G1 static budget: zero heap allocation, client state $\le 40$ KiB, stack frames $\le 1024$ bytes (with authorized 2400B RSA exception).
- **Trust Store & Anchors:** Curated offline compiler generating 120 compiled NSS root anchors in `/etc/ssl/certs/anchors.v1`.
- **HTTPS Downloader:** `/bin/wget` HTTPS support with strict HTTP 200 enforcement, downgrade refusal, chunked transfer decoding, and authenticated `close_notify` teardown commit condition. Verified across 16 deterministic adversarial gates and BadSSL live endpoints.

### ⚙️ Service Supervision & System Logging
- **Watchtower Supervisor:** `/sbin/watchtower` PID-1 service manager and `/bin/service` CLI supporting declarative `/etc/watchtower/*.service` units, dynamic discovery (`SYS_LIST_DIR`), and clean shutdown coordination.
- **Three Pillars Logging Foundation:** Forensic boot snapshot (`/bin/boot-snapshot` into `/var/log/boot.log`), supervisor non-blocking pipe capture with async gzip rotation, and standard `/bin/logger` timeline logging with scheduled `/sbin/logrotate` via crond.

### 🔐 Native SSH (SSH)
- **Secure Transport:** Ed25519 host identities, Curve25519 key exchange, and ChaCha20-Poly1305 encryption, with password and Ed25519 public-key authentication.
- **Interactive Sessions:** Privilege-separated monitor/workers, PTYs, terminal resizing, session limits, and bounded teardown; automated OpenSSH acceptance under BIOS and UEFI.
- **Client Compatibility:** PowerShell OpenSSH works, and MobaXterm 25.3 login, `top`, ordinary commands, and sudo are user-confirmed on the laptop.
- **Live USB Setup:** Explicit temporary host keys and an operator password in RAM, discarded on reboot. SSH requires explicit startup. See the [SSH guide](docs/subsystems/ssh.md) for commands and verification limits.

### 🛡️ Permissions & Security
- **Unix DAC:** Traditional UID, GID, and standard Unix file permission bits (`rwxrwxrwx`).
- **Security Tools:** `/bin/login`, `/bin/sudo`, setuid execution, and fine-grained POSIX-style process capabilities (`CAP_NET_BIND`, `CAP_SYS_ADMIN`, etc.).
- **Deterministic Cryptography:** SHA-256 boot-secret ISN generation with hardware entropy harvesting.

### 💻 Userland & Shell
- **Scripting:** Explicit `sh file` / `sh -c`, positional arguments, conditions, while/until/for loops, bounded source/return and `test`/`[`. Host sanitizers and disk-free BIOS/UEFI checks pass; bounded Dell console/SSH scripting smoke acceptance is user-confirmed. See [scripting evidence](docs/roadmap/shell-scripting.md).
- **Interactive Shell:** Modular Ring 3 command shell supporting job control (`&`, `jobs`, `fg`, `kill`), pipelines (`|`), input/output redirection (`>`, `<`), and background redraw.
- **Visual Editor:** Ring 3 visual text editor (`/bin/nano` v2.1) with Pike VM regex search, syntax highlighting, and buffer editing.
- **System Utilities:** Introspection tools (`/bin/ps`, `/bin/top`, `/bin/sysinfo`, `/bin/free`, `/bin/disk`), networking tools (`/bin/ping`, `/bin/traceroute`, `/bin/nc`, `/bin/wget`, `/sbin/dhcpc`, `/bin/dhcp-probe`), and stream processing tools (`grep`, `sort`, `uniq`, `diff`, `patch`, `sed`, `awk`, `wc`, `xxd`).
- **Time & Compression:** Native clock/date/sleep/time tools, DHCP v2 background renewal, and interoperable gzip/gunzip utilities.

### 💿 Persistent Root & NVMe Installer
- **Text-Mode Installer:** Complete installer (`/bin/install`) providing automated GPT disk partitioning, ext4 root deployment, and configuration.
- **Persistent Root:** Bootable persistent root partition, user account creation, machine hostname configuration, authenticated recovery, and clean shutdown.
- **Hardware Acceptance:** Fully tested, confirmed, and physically accepted on bare-metal Dell Latitude hardware.

---

## Current Status

| Subsystem | Status | Scope & Hardware Evidence |
| :--- | :---: | :--- |
| **Boot & Architecture** | **COMPLETE** | UEFI & BIOS boot via Limine; COM1 & framebuffer diagnostics. |
| **Multi-Core (SMP)** | **COMPLETE** | 8 logical cores, work-stealing scheduler, TLB shootdown. Dell 5590 verified. |
| **Memory Management** | **COMPLETE** | 32 GiB PMM, VMM 4-level paging, deferred CR3/PML4 reaping. |
| **ext4 Filesystem & Hardening**| **COMPLETE** | Journaled ext4 with JBD2 crash recovery; Hardening EH-1 through EH-6 (atomic `SYS_RENAME_REPLACE`, 32-block staging batches, directory relocation). Dell physical acceptance confirmed. |
| **NVMe & USB Storage** | **COMPLETE** | NVMe 1.4 driver + xHCI BOT/SCSI USB storage. |
| **Writable `/etc` (ETCFS)** | **COMPLETE** | Live-media in-memory writable configuration tree with 16 MiB quota ceiling and COW TarFS backing. |
| **Permissions & Security** | **COMPLETE** | Unix DAC, UID/GID, sudo, setuid, and capability policies. Dell verified. |
| **Userland & Shell** | **COMPLETE** | Ring 3 shell, job control, pipelines, scripting, `/bin/nano`, `/bin/free`, stream tools. |
| **Persistent Root & Installer** | **COMPLETE** | Phases 0–4 complete. Dell spare-drive install & persistent boot verified on bare metal. |
| **Network SMP (CPU-0 Handoff)**| **COMPLETE through Phase 4** | Cross-core control, UDP/DNS, TCP streams/listeners, and ordinary unpinned children. Bounded Dell physical acceptance; Phase 5 throughput redesign deferred. Consolidated `/etc/network/network.conf` and `/etc/network/resolv.conf`. |
| **SSH** | **COMPLETE through Phase 8** | Privilege-separated monitor/worker, PTY duplex bridge, OpenSSH client pubkey & password auth, BIOS/UEFI QEMU 100% PASS; MobaXterm laptop confirmed. |
| **Watchtower Service Supervisor** | **COMPLETE (Phases 0–5)** | `/sbin/watchtower` supervisor, `/bin/service` CLI, dynamic discovery via `SYS_LIST_DIR`, pipe capture, and clean shutdown. |
| **System Logging (Three Pillars)** | **COMPLETE (Phases 0–4)** | Boot snapshot (`/bin/boot-snapshot`), supervisor pipe capture with async gzip, `/bin/logger`, `/var/log/messages`, scheduled `/sbin/logrotate` via crond. |
| **TLS / HTTPS Engine** | **COMPLETE (Phases 0–5)** | BearSSL 0.6 client profile under Gate G1 static budget, 120 compiled NSS root anchors, `/bin/wget` HTTPS download with authenticated `close_notify`. |
| **Time, DHCP v2 & Compression** | **COMPLETE** | Clock/date/sleep/time, RFC 2131 background lease renewal daemon, RFC 1951/1952 gzip/gunzip. |
| **Package Management (PKG-1)** | **IN PROGRESS** | Declarative package format, catalog parsing, SHA-256 validation, and Ring 3 CLI tooling. |

### Recently Finished (2026-10-21)
- **Network Resolver Consolidation:** Relocated persistent resolver configuration to `/etc/network/resolv.conf` alongside `/etc/network/network.conf`, with transparent fallback to `/etc/resolv.conf`.
- **Live Media Writable `/etc` Filesystem (ETCFS):** In-memory mutable configuration tree with 16 MiB quota ceiling, COW TarFS backing, atomic replacement, and strict credential immutability.
- **TLS / HTTPS Engine (Phases 0–5):** Complete BearSSL 0.6 platform integration under Gate G1 static memory bounds, 120 compiled root trust anchors in `/etc/ssl/certs/anchors.v1`, 16 deterministic adversarial gates, BadSSL live test suite, and `/bin/wget` HTTPS extension.
- **System Logging Foundation (Phases 0–4):** Three Pillars architecture spanning `/bin/boot-snapshot` forensic pre-rotation, Watchtower supervisor non-blocking pipe capture with async gzip, and `/bin/logger` with crond scheduled `/sbin/logrotate`.
- **ext4 Filesystem Hardening (EH-1 through EH-6):** Atomic replacement (`SYS_RENAME_REPLACE` 84), 32-block staging writes (`SYS_STAGE_WRITE` 85), interactive SSH latency SLA gate, and directory relocation/rename (EH-6).

### Current Checkpoint: Native Package Manager (PKG-1)

Current development is focused on the native package manager (`/bin/pkg`), implementing declarative package archive packaging, SHA-256 content verification, dependency catalog resolution, and safe atomic installation.

### Downstream Roadmap
- Userland multithreading (`pthreads`)
- MicroPython platform runtime
- Broader cross-core IPC and message queues
- Deferred network throughput redesign (Phase 5)

---

## Design Principles

- **Explicit Ownership:** Every resource (buffer, lock, file descriptor, endpoint) has a defined owner and lifetime. No borrowed pointers cross ownership boundaries.
- **Bounded Failure Paths:** No unbounded retry loops, no silent degradation, and no dynamic heap allocation in critical I/O hot paths.
- **Evidence Over Claims:** Automated test suites distinguish host sanitizer mocks (ASan/UBSan/TSan), QEMU virtualized runs, and physical bare-metal hardware acceptance.
- **Strict Invariants:** Lock ranks (Ranks 1–5), no locks across context switches, and interrupt-frame preservation are strictly enforced.

---

## Getting Started

### Prerequisites

FortressOS builds in Linux environments or Windows Subsystem for Linux (WSL `Ubuntu-24.04`).

```bash
sudo apt-get update
sudo apt-get install -y build-essential nasm xorriso qemu-system-x86 ovmf \
    git curl e2fsprogs python3 gdb mtools dosfstools
```

### Build

```bash
git clone https://github.com/Pride1922/FortressOS.git
cd FortressOS
make
```

The main USB deliverable is **one raw image, `bin/fortress.img`**, with exactly
two boot choices:

1. **FortressOS (Live USB)** — interactive login, temporary runtime logs and
   writable persistent USB storage at `/mnt`.
2. **FortressOS (Installer)** — the full NVMe installer, including its deployment
   payload and explicit disk selection/confirmation.

Build it with `make img` or `make`. The combined image is 592 MiB because it
includes the installation payload, boot files, installer log slot and a separate
64 MiB live-data filesystem. `bin/fortress.iso` remains available for live QEMU
development; it is not the combined USB deliverable. The separate `make installer`
workbench remains available for specialized installer tests.

See [combined image verification](docs/roadmap/combined-usb-image.md) for the
BIOS/UEFI checks and their limits.

---

## Running FortressOS

### Running in QEMU

```bash
make run            # Boot hybrid ISO under UEFI in QEMU (q35, 2 GiB RAM, COM1)
make run-bios       # Boot hybrid ISO under legacy BIOS
make run-img        # Boot raw disk image (bin/fortress.img) under UEFI
make run-img-bios   # Boot raw disk image under legacy BIOS
make run-img-usb    # Boot raw disk image emulated as a USB mass storage flash drive
make debug          # Launch QEMU paused with GDB server on localhost:1234
```

To attach GDB:
```bash
gdb bin/fortress.elf -ex "target remote :1234" -ex "break _start" -ex "continue"
```

### Flashing to a Physical USB Drive

Flash **`bin/fortress.img`** to the USB drive (using raw/DD mode in Rufus on
Windows or `dd` on Linux). The same stick offers both Live USB and Installer:

```bash
sudo dd if=bin/fortress.img of=/dev/sdX bs=4M status=progress conv=fdatasync
```

> [!WARNING]
> Ensure `/dev/sdX` is the correct target drive. All existing data on the device will be overwritten.

The default image contains a FAT32 EFI system partition (Limine, kernel, initramfs) and a 64 MiB ext4 data partition (read-only by default; writable when booted with `usb_data_mode=rw`).

---

## Repository Structure

```text
src/
├── arch/x86_64/     CPU bring-up, GDT/IDT, APIC, IPI, context switching, syscall entry
├── kernel/          Scheduler, processes, ELF loader, handoff queue, lock discipline
├── mm/              Physical memory (PMM), virtual paging (VMM), kernel heap
├── net/             TCP/IP stack: Ethernet, ARP, IPv4, ICMP, UDP, TCP, DNS resolver
├── drivers/         PCI, NVMe, xHCI, USB BOT/SCSI, e1000/e1000e/I219-LM, console, power
├── fs/              VFS, tar initramfs, GPT parser, ext2, journaled ext4 (JBD2)
└── include/         Shared kernel and userland ABI headers (syscalls, termctl, block info)

user/
├── shell/           Interactive shell engine (line editing, job control, history, UI)
├── nano/            Ring 3 visual text editor (nano v2.1, Pike VM regex, nanorc)
├── sshd/            Native SSH transport, authentication, channels and daemon
├── tools/           Core utilities: disk, stream tools (grep, sed, awk, sort, uniq, wc), useradd, tar
└── *.c              Freestanding binaries: init, ping, udptest, nc, tcpserve, wget, top, ps

docs/
├── plans/           Architecture specifications and staged implementation roadmaps
├── subsystems/      Subsystem references (smp.md, net.md, storage.md, shell.md, usb.md)
└── roadmap/         Historical verification ledgers, checkpoints, and hardware evidence
```

---

## Documentation Index

- **[`AGENTS.md`](AGENTS.md):** Architecture guide, coding standards, lock ranks, and invariant rules *(authoritative)*.
- **[`PROTECTED.md`](PROTECTED.md):** Guardrail contracts for boot, synchronization, address spaces, and drivers.
- **[`COMMANDS.md`](COMMANDS.md):** Complete reference for shell builtins, user tools, and administrative utilities.
- **[`ARCH_REVIEW.md`](ARCH_REVIEW.md):** Cross-cutting audit findings, limitations, and technical debt.
- **[`docs/plans/`](docs/plans/):** Active implementation plans and design specifications.
- **[`docs/subsystems/`](docs/subsystems/):** Deep-dive subsystem reference annexes.
- **[`docs/roadmap/`](docs/roadmap/):** Checkpoint history, automated test logs, and bare-metal hardware evidence.

---

<p align="center">
  <sub>FortressOS is under active development. See <a href="AGENTS.md">AGENTS.md</a> and <a href="docs/roadmap/README.md">docs/roadmap/</a> for detailed technical reports and verifiable evidence ledgers.</sub>
</p>
