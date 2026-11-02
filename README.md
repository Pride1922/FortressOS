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
- **Hardware Drivers:** Intel e1000, e1000e, physical Intel I219-LM (autonomous PHY renegotiation, cold-plug and flap recovery), and Realtek RTL8153 USB 3.0 Gigabit Ethernet.
- **Cross-Core Networking:** Application calls from any CPU route through the CPU-0 protocol owner. Phases 0–4 are complete through bounded Dell physical acceptance; throughput redesign remains deferred. See [network SMP evidence](docs/roadmap/network-smp-phase4.md).

### 📶 Wireless & Wi-Fi Subsystem (802.11 / WPA2-PSK / iwlwifi)
- **802.11 MAC Engine & Bridging:** Full IEEE 802.11 management frame parsing/generation, LLC/SNAP encapsulation/decapsulation bridging to standard Ethernet 802.3, and BSS scan table across 2.4 GHz and full 5 GHz channels.
- **WPA2-PSK 4-Way Handshake:** Hardware-accelerated and software fallback PBKDF2-HMAC-SHA1 (4096 rounds), IEEE 802.11i PRF-512 PTK derivation, NIST AES Key Wrap (RFC 3394), AES-128-CCMP frame encryption/decryption, dynamic group cipher (CCMP/TKIP) and PMF/MFPC capability mirroring across Assoc Req and EAPOL M2, and monotonic 48-bit Packet Number (PN) replay attack protection.
- **Intel iwlwifi Driver:** Native PCI driver supporting Intel Dual Band Wireless-AC 8265 (Dell Latitude 5590) and Intel Wi-Fi 6E AX211 (Dell Latitude 5530), official Intel microcode loading (`iwlwifi-8265-36.ucode.gz`), and DTIM power management (10ms idle sleep windows).
- **Architectural Transformation:** Non-blocking asynchronous connection state machine (zero kernel spinning), autonomous background frame retransmission & 6s fail-closed timeout, fast-path PMK caching (sub-millisecond reconnect), multi-BSSID best-signal selection, autonomous roaming ($\ge +15\text{ dBm}$ delta threshold), unicast Wi-Fi DHCP, hardened root credential storage (`/etc/network/wlan.conf` mode 0600) with GUI auto-persistence, and `/bin/wifi` CLI with animated scan progress bar. See the [Wireless guide](docs/subsystems/wifi.md).

### 🖥️ Graphical Desktop & Compositor
- **Window Server (`/bin/compositor`):** Freestanding Rust compositor with shared-memory client surfaces (`SYS_SHM_CREATE`/`MAP`/`UNMAP`), bounded dirty-rectangle damage composition, window snapping, and runtime display scaling (1080p native / 2x UHD) persisted in `/etc/desktop.conf`.
- **Isolated Client Apps & IPC:** External client processes (`/bin/desktop-terminal` VT/ANSI terminal emulator and `/bin/desktop-files` file manager) alongside built-in desktop apps (Notepad, Calculator, Paint, Settings, System Monitor) with cross-app open-in-editor IPC (`EV_OPEN_FILE`).
- **Direct Kernel Keyboard Input & SAK:** Dedicated `/dev/input/keyboard` (`/dev/kbd0`) character device delivering 8-byte `key_event_t` packets (Linux keycode, modifier bitmask, press/release/repeat state, and translated ASCII) from PS/2 and USB HID drivers with console TTY muting while grabbed. Supports extended `EV_KEY` protocol delivery, global `Alt+Tab` window switching, `Super` launcher toggle, and `Ctrl+Alt+Esc` Secure Attention Key (SAK) interception. See the [Desktop guide](docs/subsystems/desktop.md).

### 📦 Package Management & Base OS Updates
- **Repository Base Updates:** Complete A/B kernel/bootstrap/base-userland updates through `pkg upgrade [-y]` on the new installer layout, with preserved settings/apps, boot confirmation and previous-set recovery. Focused host acceptance passes; firmware/physical acceptance remains pending. [Delivery and publisher workflow](docs/base-repository-updates.md).
- **Native Package Manager (`/bin/pkg` & `SYS_PKGCTL`):** Declarative PKG-1 archive verification, Ed25519 signature & SHA-256 content validation, catalog resolution, verified reinstall idempotence, status inspection, and atomic application upgrades (`PKG-3`).
- **Offline Kernel Updates (`/bin/kernel-update`):** Predecessor-bound two-slot (`slot A` / `slot B`) kernel update workflow preserving the installed bootstrap and root filesystem from disposable USB update media with byte-exact root preservation, rollback protection, and authenticated recovery boot entries.

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

### 💻 Userland, Threading & Shell
- **Userland Threads (`pthreads`):** Shared-address-space multithreading (`SYS_THREAD_CREATE`, `SYS_THREAD_EXIT`, `SYS_THREAD_JOIN`, `SYS_THREAD_SELF`, `SYS_THREAD_YIELD`, `SYS_FUTEX`, syscalls 89–94) and freestanding `<pthread.h>` mutexes, condition variables, barriers, and once-initialization.
- **Scripting:** Explicit `sh file` / `sh -c`, positional arguments, conditions, while/until/for loops, bounded source/return and `test`/`[`. Host sanitizers and disk-free BIOS/UEFI checks pass; bounded Dell console/SSH scripting smoke acceptance is user-confirmed. See [scripting evidence](docs/roadmap/shell-scripting.md).
- **Interactive Shell:** Modular Ring 3 command shell supporting job control (`&`, `jobs`, `fg`, `kill`), pipelines (`|`), input/output redirection (`>`, `<`), and background redraw.
- **Visual Editor:** Ring 3 visual text editor (`/bin/nano` v2.1) with Pike VM regex search, syntax highlighting, and buffer editing.
- **System & Network Utilities:** Introspection tools (`/bin/ps`, `/bin/top`, `/bin/sysinfo`, `/bin/free`, `/bin/disk`), networking tools (`/bin/ping`, `/bin/traceroute`, `/bin/nc`, `/bin/wget`, `/bin/ssh`, `/bin/telnet`, `/bin/httpd`, `/sbin/dhcpc`, `/bin/dhcp-probe`), `SYS_POLL` readiness multiplexing, and stream processing tools (`grep`, `sort`, `uniq`, `diff`, `patch`, `sed`, `awk`, `wc`, `xxd`, `base64`).
- **Time & Compression:** Native clock/date/sleep/time tools, IANA TZif timezone engine, DHCP v2 background renewal, and interoperable gzip/gunzip utilities.

### 💿 Persistent Root & NVMe/SATA Installer
- **Text-Mode Installer:** Complete installer (`/bin/install`) providing automated GPT disk partitioning, ext4 root deployment, and configuration across NVMe and SATA/AHCI targets.
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
| **NVMe, SATA/AHCI & USB Storage** | **COMPLETE** | NVMe 1.4 driver, SATA/AHCI controller engine, and xHCI BOT/SCSI USB storage + multi-tier hubs. |
| **Writable `/etc` (ETCFS)** | **COMPLETE** | Live-media in-memory writable configuration tree with 16 MiB quota ceiling and COW TarFS backing. |
| **Permissions & Security** | **COMPLETE** | Unix DAC, UID/GID, sudo, setuid, and capability policies. Dell verified. |
| **Userland, `pthreads` & Shell** | **COMPLETE** | Ring 3 shell, job control, pipelines, scripting, `pthreads` (syscalls 89–94), crash dumps, `/bin/nano`, `/bin/free`, stream tools. |
| **Persistent Root & Installer** | **COMPLETE** | Phases 0–4 complete + offline two-slot kernel updates (`/bin/kernel-update`). Dell spare-drive install & persistent boot verified on bare metal. |
| **Network SMP (CPU-0 Handoff)**| **COMPLETE through Phase 4** | Cross-core control, UDP/DNS, TCP streams/listeners, and ordinary unpinned children. Bounded Dell physical acceptance; Phase 5 throughput redesign deferred. Consolidated `/etc/network/network.conf` and `/etc/network/resolv.conf`. |
| **Wireless & Wi-Fi (802.11 / WPA2-PSK / iwlwifi)** | **COMPLETE (Phases 0–4 + Transformation)** | IEEE 802.11 MAC engine, LLC/SNAP bridging, WPA2-PSK 4-way handshake, dynamic group cipher & PMF/MFPC negotiation, full 5 GHz scan channels, unicast Wi-Fi DHCP, AES-128-CCMP, Intel AC 8265 & AX 211 driver, async non-blocking connect, multi-BSSID best-signal selection, autonomous roaming, PMK caching, DTIM power management, and `/etc/network/wlan.conf` mode 0600 root credential store. |
| **SSH Server & Client** | **COMPLETE through Phase 8 + `/bin/ssh`** | Privilege-separated `/sbin/sshd` monitor/worker, PTY duplex bridge, standalone `/bin/ssh` client, OpenSSH pubkey & password auth, BIOS/UEFI QEMU 100% PASS; MobaXterm laptop confirmed. |
| **Watchtower Service Supervisor** | **COMPLETE (Phases 0–5)** | `/sbin/watchtower` supervisor, `/bin/service` CLI, dynamic discovery via `SYS_LIST_DIR`, pipe capture, and clean shutdown. |
| **System Logging (Three Pillars)** | **COMPLETE (Phases 0–4)** | Boot snapshot (`/bin/boot-snapshot`), supervisor pipe capture with async gzip, `/bin/logger`, `/var/log/messages`, scheduled `/sbin/logrotate` via crond. |
| **TLS / HTTPS Engine** | **COMPLETE (Phases 0–5)** | BearSSL 0.6 client profile under Gate G1 static budget, 120 compiled NSS root anchors, `/bin/wget` HTTPS download with authenticated `close_notify`. |
| **Time, DHCP v2 & Compression** | **COMPLETE** | Clock/date/sleep/time, IANA TZif timezones, RFC 2131 background lease renewal daemon, RFC 1951/1952 gzip/gunzip, RFC 4648 base64. |
| **Package Management (`PKG-1` / `PKG-3`)** | **COMPLETE** | `SYS_PKGCTL` (syscall 95), `/bin/pkg` CLI, Ed25519/SHA-256 verification, atomic application upgrades, reinstall idempotence, and predecessor-bound offline kernel updates. |
| **Graphical Desktop & Compositor** | **COMPLETE** | `/bin/compositor` (`fortress-desktop`), SHM client surfaces, isolated `/bin/desktop-terminal` and `/bin/desktop-files` processes, direct `/dev/input/keyboard` (`key_event_t`) input with PS/2 + USB HID modifier/press/release tracking, extended `EV_KEY`, `Alt+Tab`, `Super`, and `Ctrl+Alt+Esc` SAK. |

### Recently Finished (2026-11-01)
- **Direct Keyboard Input, Extended `EV_KEY` & Secure Attention Key (SAK):** Kernel `/dev/input/keyboard` (`/dev/kbd0`) device with 8-byte `key_event_t` packets (keycode, modifiers, press/release/repeat state, ASCII) fed by PS/2 and USB HID drivers, console TTY muting while grabbed, extended 64-byte `EV_KEY` desktop protocol, `Alt+Tab` / `Super` compositor shortcuts, and `Ctrl+Alt+Esc` SAK interception.
- **Isolated Desktop Client Processes & Polish:** Standalone `/bin/desktop-terminal` and `/bin/desktop-files` client processes with open-in-editor IPC (`EV_OPEN_FILE`), VT/ANSI binary-output redraw resilience, and 1280x800 Settings integration.
- **Wi-Fi WPA2-PSK Interoperability & 5 GHz Expansion:** Dynamic group cipher (CCMP/TKIP) and PMF/MFPC capability mirroring in both Association Request and EAPOL Message 2, full 5 GHz channel scan table, unicast Wi-Fi DHCP, and GUI profile auto-persistence.
- **Atomic Package Upgrades (`PKG-3`) & Offline Kernel Updates:** Verified atomic application upgrades (`rot13` 1.0 → 2.0), reinstall idempotence, status/session-acquire locking, and predecessor-bound two-slot offline kernel updates (`/bin/kernel-update`).
- **Userland Threads (`pthreads`), Crash Dumps & Network Tools:** Shared-address-space threads (`SYS_THREAD_CREATE`..`SYS_FUTEX`), bounded Ring 3 crash core dumps, `/bin/ssh` client, `/bin/telnet`, `/bin/httpd`, and `SYS_POLL` readiness multiplexing.

### Downstream Roadmap
- Extended `SYS_LIST_DIR_EXT` directory continuation ABI for >63-byte filenames and arbitrary-sized directories in `desktop-files`
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
├── kernel/          Scheduler, processes, threads/futex, ELF loader, poll, handoff queue
├── mm/              Physical memory (PMM), virtual paging (VMM), SHM, kernel heap
├── net/             TCP/IP stack (ETH/ARP/IPv4/ICMP/UDP/TCP/DNS) and 802.11/WPA2-PSK Wi-Fi
├── drivers/         PCI, NVMe, AHCI, xHCI, USB BOT/HID/Hub, e1000/I219, RTL8153, iwlwifi, kbd/mouse
├── fs/              VFS, tar initramfs, ETCFS, GPT, ext2, journaled ext4 (JBD2), PKG backend
└── include/         Shared kernel and userland ABI headers (syscalls, desktop_proto, input_event)

user/
├── compositor/      Rust desktop window server (/bin/compositor) and client apps (terminal, files)
├── pkg/             Native package manager (/bin/pkg) and catalog/signature verification tooling
├── shell/           Interactive shell engine (line editing, job control, scripting, history)
├── nano/            Ring 3 visual text editor (nano v2.1, Pike VM regex, nanorc)
├── sshd/            Native SSHv2 server (/sbin/sshd) and standalone client (/bin/ssh)
├── tools/           Core utilities: disk, stream tools (grep, sed, awk, sort, uniq, wc), kernel-update
└── *.c              Freestanding binaries: init, ping, traceroute, nc, wget, httpd, telnet, top, ps

docs/
├── plans/           Architecture specifications and staged implementation roadmaps
├── subsystems/      Subsystem references (smp.md, net.md, wifi.md, desktop.md, storage.md, shell.md, usb.md)
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
