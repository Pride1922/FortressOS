# FortressOS

**A 64-bit operating system built from the kernel up.**

FortressOS is a freestanding C11 and x86-64 assembly kernel with isolated user
processes, an interactive shell, a writable ext2 filesystem, and a from-scratch
TCP/IP stack. It boots through [Limine](https://github.com/limine-bootloader/limine) under UEFI or legacy BIOS, runs in QEMU,
and has been tested on real Dell Latitude 5590 and 5530 hardware.

It is an independent kernel, not a Linux distribution. The project is still
under active development: its emphasis is on explicit ownership, bounded failure
paths, and tests that distinguish emulator results from hardware observations.

> This README is a human-facing overview and may lag implementation by a commit or
> two. For authoritative, currently-accurate status, implementation contracts, and
> what's safe to change, read [`AGENTS.md`](AGENTS.md) and [`PROTECTED.md`](PROTECTED.md) — those are kept in sync
> with the code, this file is kept in sync with those.

Current milestone: Networking NET-1, NET-2, runtime link recovery, and NET-3 (runtime network configuration) are
complete. The stack covers the Intel e1000/e1000e/I219-LM driver, Ethernet II
framing, ARP, IPv4 unicast, ICMP Echo (/bin/ping), UDP sockets (/bin/udptest),
TCP streams (echoc/echos/tcpserve/nc), a userspace DNS resolver (nslookup,
hostname nc), cold cable insertion with retained-ring link recovery, and runtime
network configuration (/bin/ifconfig, /bin/ifup, /mnt/.fortress/network.conf).
Physical acceptance is recorded on the Dell Latitude 5590 for ICMP, UDP, TCP,
DNS, link recovery (all six cold-wait, insertion, unplug/replug, and listener
error cases), and runtime network configuration (/bin/ifconfig, /bin/ifup,
on-the-fly CLI reconfig, /mnt/.fortress/network.conf, and DNS fallback).
Shell milestones S0–S9 are complete: editing, history, expansion,
redirection, pipes, jobs, signals, process groups, terminal foreground ownership,
and introspection (ps, top, sysinfo) verified on QEMU BIOS/UEFI and physical
Dell hardware.

## What works today

| Area | Implemented capabilities |
| --- | --- |
| Boot & Visual UX | Quiet graphical boot splash with custom FortressOS logo (Tokyo Night color palette) and live centered status ticker. Raw boot diagnostics suppressed on framebuffer by default (100% preserved in COM1 serial and in-memory dmesg). Automatic console un-mute and context dump on [FAIL] or [PANIC]. Clean screen transition into Ring 3 shell. Limine menu supports standard quiet boot and FortressOS (Verbose Debug) fallback. |
| Multi-Core (SMP) | 8-core concurrent execution verified on bare metal. AP discovery via Limine/ACPI MADT (Piece 1); per-CPU GS base, GDT, TSS, and IST stacks (Piece 2); strict rank-checked lock discipline with contention telemetry and panic isolation (Piece 3); distributed multi-core preemptive scheduler with per-CPU runqueues and dual-lock work-stealing (Piece 4); APIC ICR cross-core IPIs and broadcast synchronous TLB shootdowns (Piece 5); and full multi-core memory architecture with contention deadlock breaking, op_refs, sched_refs, and deferred address-space reaping (Piece 6). |
| Memory | Physical page allocator covering 32 GiB RAM with two-stage boot initialization (Phase 9H / Piece 6A); concurrent PMM allocation safety verified across 320,000 cycles under 622k+ contention events (Piece 6B); contention-safe TLB shootdown and CR3 reload (Piece 6C); per-process address spaces with transient operation references (op_refs), scheduler references (sched_refs), hardware active CPU masks, and guaranteed zero-leak deferred destruction (Piece 6D). |
| CPU and scheduling | GDT/IDT per CPU, exception diagnostics, dedicated double-fault/NMI stacks, ACPI discovery, APIC timer preemption (100 Hz), per-CPU runqueues, and work-stealing across online cores. |
| User programs | Ring 3 execution, ELF loading, fast syscall MSRs configured across all cores, System V AMD64 argument passing, validated syscalls, cross-core child waiting, exit status propagation, and deferred process reclamation. |
| Storage | PCI discovery, NVMe reads/writes/flush, xHCI + USB Mass Storage BOT (USB 2.0 and USB 3.x SuperSpeed), validated GPT partitions, and bounded read/write ext2 support. Multi-core concurrent append serialization verified under QEMU -smp 4 and Dell Latitude 5590 hardware with offline e2fsck -fn audits. Write persistence is verified on QEMU NVMe fixtures and on two independent physical USB devices. |
| USB | Multiple xHCI controllers enumerated and initialized; device enumeration and descriptor parsing; BOT/SCSI reads and writes; durability classification with per-device policy; explicit writable opt-in. Both USB 2.0 and directly-attached USB 3.x (SuperSpeed) devices are supported; external hubs and hot-plug are not. |
| Files | Read, create, write, truncate, make directories, rename/move, and delete. Initramfs provides boot-time programs; ext2 provides persistent storage. |
| Networking — driver | Intel e1000 (82540EM), e1000e (82574L), and integrated I219-LM driver with polling-only ingress on a BSP-pinned worker. The same descriptor layout serves QEMU and bare metal. I219 physical acceptance on the Dell Latitude 5590 (8086:15D7) and 5530 (8086:1A1E). DMA quarantine on controller fault. No NIC interrupt handlers or MSI vectors in this milestone. |
| Networking — protocols | Ethernet II framing; ARP request/reply with reply-only cache learning; IPv4 unicast with header validation; ICMP Echo Request/Reply; UDP with a bounded 16-socket table; TCP with Reno slow start, congestion avoidance, fast retransmit, SRTT/RTTVAR RTO and Karn's rule; a userspace DNS stub resolver. All bounded, static, and BSP-owned. |
| Networking — ABI | SYS_NETCTL=42 (ping, NETCTL_IFGET, NETCTL_IFSET, NETCTL_TRACE_PROBE); SYS_SOCKET/BIND/SENDTO/RECVFROM = 38–41 (UDP); SYS_CONNECT/LISTEN/ACCEPT/SEND/RECV/SHUTDOWN = 43–48 (TCP); SYS_SEND_UNTIL/RECV_UNTIL/CONNECT_UNTIL = 49–51 (opt-in absolute BSP-tick deadlines, max 60 s horizon). All BSP-only, explicitly rejecting AP callers. |
| Networking — tools | /bin/ifconfig, /bin/ifup, /bin/ping, /bin/traceroute, /bin/udptest, /bin/echoc, /bin/echos, /bin/tcpserve, /bin/nc (serial request/response with hostname resolution), /bin/nslookup, /bin/wget, /bin/download. |
| New tools — physical acceptance pending | download, wget, streaming md5sum/sha256sum, traceroute and nano have automated test evidence; Dell hardware tests remain pending. Tar extraction awaits the changed-archive policy decision. |
| Networking — physical acceptance | Dell Latitude 5590: ICMP (4/4, matched request/reply pairs, second-host Wireshark screenshot), UDP (user-reported PASS, capture audit pending), TCP (10/10 QEMU matrix; physical acceptance for client, real HTTP server, guest listener, and RST recovery), DNS (10/10 QEMU matrix; physical acceptance for A, CNAME, NXDOMAIN, TC→TCP fallback, stall timeout, and hostname nc), link recovery (cold waiting, autonomous PHY renegotiation, retained-ring replug, 6/6 PASS), and runtime network configuration (ifconfig query, on-the-fly ifup, persistent config apply, ping gateway, and DNS fallback PASS). Dell Latitude 5530: driver bring-up and ICMP. |
| Shell (Milestones S0–S9) | Modular Ring 3 shell (user/shell/) featuring 4096-byte line editing, horizontal viewport, cursor movement, Ctrl shortcuts, RAM history, incremental Ctrl+R search, bracketed paste review, raw/timed input (SYS_INPUT_READ), terminal mode control (SYS_TERMCTL), Belgian AZERTY AltGr operator decoding, working directories (cd/pwd), logic chaining (;, &&, ||, !), parameter expansion ($VAR, ${VAR}, $?), aliases, globbing, uniform descriptors (0–31), redirections (<, >, >>, 2>&1, n>&-), retained UI terminal handle (fd 31 with CLOEXEC), version builtin, persistent history (/mnt/.fortress/history), pipes and stream utilities, job control (jobs, fg, bg, kill), signals (SIGINT, SIGPIPE, SIGTSTP, SIGCONT, SIGCHLD), process groups, terminal foreground ownership, and introspection utilities (ps, top, sysinfo). |
| Power and platform | BIOS/UEFI boot images, ACPI S5 shutdown, and reset fallbacks. Shutdown and reboot verified on bare-metal Dell Latitude 5590. |

User-process fault isolation and resource reclamation have targeted tests; this
is not a claim of complete security isolation. ext2 writes are bounded to direct
and single-indirect blocks; see `ARCH_REVIEW.md` § "ext2 write cap" for the exact
limit and the ext4 target. The filesystem does not promise crash-atomic updates
or recovery from arbitrary power loss.

USB durability is classified per device and disclosed in the boot log. A device
that reports its caching page and accepts SYNCHRONIZE CACHE gets the strong
guarantee; a device that reports neither is mounted write-through on the
assumption that it behaves like every other consumer stick, with an explicit
warning that power loss during writes may lose data. Physical power-loss
tolerance is not claimed for any device, even a SYNC_BACKED one — clean shutdown
is what's verified.

Networking is polling-only: no NIC interrupt handlers or MSI vectors are
registered. The net_worker thread polls the RX ring on the BSP, woken by an
authorized BSP timer hook. Cross-core socket access is deferred. The stack is
single-BSP-owner and non-reentrant. TCP's ISN generation is deliberately
non-cryptographic; a 120-second TCP-only reboot quiet period after reboot bounds the
cross-boot hazard (RFC 9293 §3.4.3 fallback). Physical acceptance covers the
Dell I219-LM path; the e1000/e1000e QEMU paths are verified in QEMU. nc is a
serial request/response tool, not a full-duplex forwarder: a peer that streams
a large response before consuming the whole request can deadlock it. The TCP
transport supports full-duplex; the nc tool does not exercise it. A readiness
primitive (select/poll) would close that gap and is deferred until two or more
tools need it.

## Try it in QEMU

Build on Linux, or use WSL **Ubuntu-24.04** on Windows. Install the tools:

```bash
sudo apt-get update
sudo apt-get install -y build-essential nasm xorriso qemu-system-x86 ovmf \
    git curl e2fsprogs python3 gdb mtools dosfstools

git clone https://github.com/Pride1922/FortressOS.git
cd FortressOS
make
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
make debug          # Start paused for GDB on port 1234
```

The `run-img*` targets attach a separate NVMe test disk whose ext2 partition supplies `/mnt` — that is the QEMU storage fixture, not the USB path. To exercise the real USB code path in QEMU, use the USB-specific runners below.

### Booting the raw image from a USB stick

`bin/fortress.img` can be flashed to a USB stick and booted on real hardware:

```bash
sudo dd if=bin/fortress.img of=/dev/sdX bs=4M status=progress conv=fdatasync
```

The image carries two partitions: a FAT32 EFI System Partition with Limine, the kernel, and the initramfs, and a 64 MiB ext2 data partition. The default boot entry mounts the data partition read-only. A second boot entry opts into a writable mount of the same partition, matched by `PARTUUID`.

On the Dell Latitude 5590, both a Kingston USB DISK 2.0 (VID `0x13FE`, PID `0x4200`, USB 2.0) and a SanDisk USB 3.2 Gen 1 (VID `0x0781`, PID `0x5588`, SuperSpeed) enumerate and mount their ext2 partitions read-write. Files written from the shell persist across a full power cycle; `e2fsck -fn` on the unmounted stick reports 0 errors. The SanDisk classifies as `SYNC_BACKED` — the strongest durability tier — verified on physical hardware.

To try saving files on the disposable QEMU NVMe fixture (not the USB path):

```bash
make run-bios WRITE_TEST=1
```

Use `shutdown` or `poweroff` in the shell to flush the filesystem and finish a clean session.

## Boot Experience & Visual Design

FortressOS features a distraction-free, modern boot experience inspired by the Tokyo Night color palette:

- **Quiet Graphical Splash:** The framebuffer displays a centered, custom-rendered FortressOS shield logo on a `#1A1B26` backdrop with a live centered status ticker updating kernel boot milestones in real time.
- **Diagnostic Preservation & Instant Troubleshooting:** Raw boot logs are suppressed on the screen by default to maintain a clean aesthetic, but 100% of diagnostic output is continuously captured in the 64 KiB in-memory `dmesg` buffer and mirrored to COM1 serial (UART). In the shell, `dmesg -n 13` or `dmesg tail 20` enables instant live troubleshooting of recent driver, network, and storage events without scrolling or rebooting.
- **Auto-Unmute Crash Protection:** If the kernel encounters any assertion failure (`[FAIL]`) or panic (`[PANIC]`), quiet mode is automatically disabled and the full diagnostic context is dumped to the screen for immediate troubleshooting.
- **Limine Bootloader Modes:**
  - `FortressOS` (default): Quiet graphical boot with live status ticker and clean handoff to the Ring 3 shell.
  - `FortressOS (Verbose Debug)`: Streams full scrolling kernel diagnostics directly to the display.
- **Clean Shell Handoff:** Once hardware and filesystem initialization completes, the screen clears smoothly and presents the interactive Ring 3 shell with Pride1922 branding.

## At the shell

> For the comprehensive reference explaining every single command, shell builtin, stream tool, network utility, and keyboard shortcut, see [`COMMANDS.md`](COMMANDS.md).

```text
version               # print kernel version, architecture, and Pride1922 branding
help                  # view available commands and builtins
pwd                   # display current working directory
cd /mnt/notes         # navigate filesystem
export FOO="bar"      # set environment variable
echo $FOO             # variable expansion
alias ll="ls -l"      # define command alias
echo "data" > out.txt # redirect stdout to file
echo "more" >> out.txt# append stdout to file
printf "abc" | wc -c  # format exact bytes without newline (RFC vectors)
cat < out.txt         # redirect stdin from file
ls /missing 2> err.log# redirect stderr
history               # display in-memory command history
jobs                  # list background jobs
fg %1                 # bring job 1 to foreground
kill -INT %1          # send SIGINT to job 1
top                   # live process view with CPU% and stable PID sorting
nano /mnt/notes.txt   # visual full-screen interactive text editor
sysinfo               # managed-RAM total, monotonic timebase, and system info
terminal local        # select output mode: local (full screen), serial, mirror, or plain
layout azerty         # Belgian AZERTY with AltGr (| \ {} [] ~)
layout us             # US QWERTY
ifconfig              # query network interface address, link state, and packet counters
ifup 10.0.2.15/24 10.0.2.2 # configure network interface statically
ping -c 4 10.0.2.2    # send four ICMP Echo Requests to the gateway
nslookup example.com  # resolve domain name to IPv4 address via DNS
wget http://example.com/index.html # download file over HTTP
download /mnt http://192.168.0.153:8000/testfile.txt # simple file download to /mnt
dmesg -n 13           # view last 13 kernel diagnostic lines (tail)
dmesg /mnt/boot.log   # dump kernel ring buffer to persistent file
run /bin/hello world  # execute user program
echo $?               # exit status of last command
```

### Interactive line editing & shortcuts

- **Navigation:** Left / Right arrows move cursor; Home (`Ctrl+A`) / End (`Ctrl+E`) jump to line boundaries.
- **Editing:** Backspace and Delete remove characters; `Ctrl+D` deletes at cursor (or exits the shell on an empty line).
- **Kill buffer:** `Ctrl+W` erases previous word; `Ctrl+U` erases to start of line; `Ctrl+K` erases to end of line; `Ctrl+Y` yanks (pastes) the last erased text.
- **History & Search:** Up / Down arrows recall commands and restore unfinished drafts; `Ctrl+R` begins reverse incremental search (Enter accepts for editing, second Enter executes; Escape or `Ctrl+G` cancels).
- **Tab Completion:** Tab completes builtin names, executables in `/bin`, and filesystem paths with smart prefix matching.
- **Control & Logic:** `Ctrl+L` clears screen and repaints; `Ctrl+C` cancels current input; command chaining with `;`, `&&`, `||`, and `!`.
- **Safety:** Command lines support up to 4096 bytes with a horizontal scrolling viewport. Input loss or overflow displays `[lost/full: Ctrl+C]` and refuses to execute partial input. Bracketed paste converts newlines to spaces and requires two Enter presses to execute (`[paste: Enter twice]`).

With the USB stick's data partition mounted read-write:

```text
mkdir /mnt/notes
cd /mnt/notes
echo "Hello from FortressOS" > hello.txt
cat hello.txt
echo "Appended log entry" >> hello.txt
edit hello.txt
mv hello.txt saved.txt
sync
shutdown
```

After reboot, `cat /mnt/notes/saved.txt` returns the exact preserved file.

## Networking quick start

With a wired connection on a LAN where 192.168.0.168 is free, 192.168.0.1
is the gateway, and DNS servers are reachable at 1.1.1.1 and 8.8.8.8, configure
the interface dynamically or via boot cmdline:

```text
ifup 192.168.0.168/24 192.168.0.1 1.1.1.1 8.8.8.8
```

ICMP ping (numeric IPv4 or domain hostname):

```text
ping -c 4 192.168.0.1
```
or ping by hostname via the userspace DNS resolver:
```text
ping -c 4 google.com
```

Expected output:

```text
PING google.com (142.250.72.14) (32 data bytes)
32 bytes from 142.250.72.14: icmp_seq=1 time=20 ms
32 bytes from 142.250.72.14: icmp_seq=2 time=20 ms
32 bytes from 142.250.72.14: icmp_seq=3 time=20 ms
32 bytes from 142.250.72.14: icmp_seq=4 time=20 ms
4 probes, 4 replies, 0% loss
rtt min/avg/max = 20/20/20 ms
```

UDP:

```text
udptest 192.168.0.222 7777 fortress-phase5
```

Expected: UDP echo PASS with matching peer bytes.

TCP (after the 120-second reboot quiet period has elapsed):

```text
echo "fortress-tcp-physical" | nc 192.168.0.222 9000
```

Expected: the peer's echo prints and the prompt returns.

DNS (with a DNS server reachable at 192.168.0.222):

```text
nslookup -s 192.168.0.222 service.test
```

Expected:

```text
Server: 192.168.0.222
Name: service.test
Address: 192.168.0.222
```

Hostname nc:

```text
echo fortress-dns | nc -s 192.168.0.222 service.test 9000
```

Expected: the peer's echo prints and the prompt returns.

The stack sends correct Ethernet, ARP, IPv4, ICMP, UDP, and TCP frames. Physical
acceptance on the Dell Latitude 5590 is recorded for each layer with
second-host Wireshark captures. TCP and DNS retain full pcapng evidence; ICMP
and UDP are recorded as second-host screenshots (no retained pcap, so no
independent payload/checksum verification is claimed for those two). There is
no DHCP client. /bin/ping accepts both numeric IPv4 and domain hostnames (resolved
via the userspace stub resolver with automatic fallback across configured servers);
nslookup, nc, wget, and download resolve hostnames via /tmp/resolv.conf or /mnt/.fortress/resolv.conf.

## Real hardware: what has been verified

Manual testing on a Dell Latitude 5590 (Core i5-8350U, 32 GiB installed RAM,
256 GB NVMe, Intel UHD 620) has confirmed USB boot, the framebuffer console,
PS/2 keyboard input, Belgian AZERTY behavior, shell interaction, argument
passing to /bin/hello, ACPI shutdown, reboot, full use of the machine's 32 GiB
of RAM, and the complete networking path: Intel I219-LM driver bring-up, ARP
resolution of the LAN gateway, a matched two-way ICMP exchange, UDP echo, TCP
client/server flows, DNS resolution (A records, CNAME chains, NXDOMAIN,
TC→TCP fallback, multi-server fallback, and hostname ping), and runtime link recovery (cold
cable waiting, autonomous PHY renegotiation on insertion, and retained-ring
continuity across flaps). A Dell Latitude 5530 (8086:1A1E) has also been
verified for driver bring-up and ICMP.

USB storage is verified on physical hardware across two device classes. The kernel discovers xHCI controllers, addresses both a USB 2.0 stick (Kingston) and a USB 3.x SuperSpeed stick (SanDisk), parses GPT on each, mounts their ext2 partitions read-write, and persists files written from the editor across a full power cycle. `e2fsck -fn` on the unmounted stick from Linux reports 0 errors. A **Dell Latitude 5530**, which exposes two independent xHCI controllers, has also been verified: both controllers initialize, and a stick is reachable and mountable on either one.

The internal physical NVMe is deliberately excluded from the USB storage mount path by parent-device provenance. Adding an ext2 partition to the laptop's SSD does not enable automatic mounting.

What is not yet verified on hardware:

- Physical power-loss tolerance for USB storage. Even a SYNC_BACKED device's
  guarantee is about a completed flush, not about surviving power loss
  mid-write. Only clean-shutdown persistence is verified on any device.
- NVMe write persistence on physical hardware. NVMe read/write/flush and ext2
  writable-mount persistence are verified against QEMU fixtures; the equivalent
  three-boot e2fsck-clean test has not yet been run against the Dell's
  internal NVMe.
- Sustained network load, line-rate throughput, and measured idle CPU. The
  ingress worker is verified for correctness, not measured throughput.
- Cross-core socket operation. All socket syscalls are BSP-only; AP callers
  receive an explicit unsupported-context error.

## USB storage and durability

The USB path is a self-contained driver set under `src/drivers/xhci*` and `src/fs/usb_mount.c`. It targets xHCI controllers only; EHCI/UHCI/OHCI are out of scope.

**Supported today:**

- Directly attached USB 2.0 (Full-Speed/High-Speed) and USB 3.x (SuperSpeed) mass-storage devices, on any enumerated xHCI controller.
- BOT transport with LUN 0 only.
- Read, write, and `SYNCHRONIZE CACHE` where the device supports them.
- Partition selection by `PARTUUID` with USB parent-device provenance.
- Explicit `usb_data_mode=rw` opt-in; the default boot entry is read-only.

**Not supported:**

- External hubs (class 0x09; rejected) — recursive/hub-attached enumeration is deferred (Phase 9G.5e) until a specific device needs it.
- Hot-plug or reconnection.
- UAS, or any device class other than mass storage.
- SuperSpeedPlus (10 Gbps) — untested.

**Durability classification** is per-device and printed in the boot log:

- `SYNC_BACKED` — the device accepts `SYNCHRONIZE CACHE`; every flush must complete. Verified on physical hardware (SanDisk USB 3.x).
- `WRITE_THROUGH` — `MODE SENSE` reports `WCE=0`; the device reports no write cache.
- `ASSUMED_WRITE_THROUGH` — the device reports neither, but is mounted RW with an explicit disclosure that power-loss during writes may lose data. Where the Kingston USB 2.0 stick lands.
- `READ_ONLY` — the device reports a write cache it cannot flush, or is in an error state.

The classification is what gates writable mount: only devices that can either prove their durability or accept the fallback disclosure get RW. A device that reports `WCE=1` and rejects `SYNCHRONIZE CACHE` is refused.

## Networking architecture

The networking subsystem is a self-contained driver and protocol stack under
src/net/ and src/drivers/e1000.c. It targets the Intel Gigabit Ethernet family:
e1000 (82540EM), e1000e (82574L), and the integrated I219-LM found on modern
Dell laptops. The same descriptor layout and MMIO register set is shared across
all three, so a single driver serves QEMU and bare metal.

Layered scope:

```text
Ring 3 user programs (/bin/ping, /bin/udptest, /bin/echoc, /bin/echos,
/bin/tcpserve, /bin/nc, /bin/nslookup)
    |  SYS_NETCTL=42 (ping); 38–41 (UDP sockets); 43–48 (TCP sockets);
    |  49–51 (opt-in absolute-deadline SEND/RECV/CONNECT)
    v
Transport: ICMP Echo; UDP; TCP (Reno); a userspace DNS stub resolver
    |
    v
Network: IPv4 unicast, ARP
    |
    v
Data link: Ethernet II framing, MAC filter
    |
    v
Network device interface (net_dev_t)
    |
    v
Intel e1000 / e1000e / I219-LM driver (polling)
```

Design constraints:

- Polling-only ingress. No NIC interrupt handlers or MSI vectors. The
  net_worker thread polls the RX ring on the BSP, woken by an authorized BSP
  timer hook that also services the terminal input worker.
- Sole BSP protocol owner. All NIC I/O, timer sweeps, and TCP state changes
  run on CPU 0. AP code may publish detach requests but must not mutate
  connections or perform NIC work. The stack is non-reentrant.
- Rank-1 network locks. g_net_dev_lock, g_socket_table_lock,
  g_tcp_endpoints_lock, the ping mailbox lock, and the socket-manager lock are
  all Rank-1 and never nest. Copy-out/drop/acquire sequencing throughout.
- Bounded static state. No dynamic allocation on hot paths. 16 socket handles,
  8 TCP connections, 4-datagram-per-socket RX queues, one wake hint per
  endpoint. No general packet queue.
- Absolute per-call I/O deadlines. SYS_SEND_UNTIL/SYS_RECV_UNTIL/
  SYS_CONNECT_UNTIL take a by-value uint64_t BSP-tick deadline. No
  socket-wide timeout option; dup/inherited descriptors cannot silently
  change another continuation's timeout. 60-second reviewable horizon.
- DMA quarantine on controller failure. Uncertain ownership means terminal
  FAILED and quarantine, never reallocation. Same shape as the storage path.
- Static IP configuration. No DHCP client. The boot cmdline accepts
  net=<IPv4>/<prefix>,<gateway> with bounded parsing and warning/default
  fallback on malformed input.
- Non-cryptographic ISN with a 120-second TCP-only reboot quiet period. The
  quiet period is the RFC 9293 §3.4.3 fallback for systems without a
  cryptographic entropy source. UDP and ICMP remain usable during the window.
- Serial nc. nc is a request/response tool: send stdin, SHUT_WR, receive
  until EOF. It is not a full-duplex forwarder. The TCP transport supports
  duplex; a readiness primitive (select/poll) would let a single-threaded
  tool exercise it and is deferred.

What is not in the current networking milestone: IPv6, Wi-Fi, TLS, DHCP,
hardware offloads (TSO/LRO/checksum), multiple NICs, bridging, bonding,
cross-core sockets, and interrupt/MSI ingress. A readiness primitive is
deferred until two or more tools need it.

## Multi-Core (SMP) Architecture

Multi-core execution is complete and verified on bare metal (Dell Latitude 5590, 8 CPUs, 32 GiB RAM). It was designed and sequenced in six pieces — architectural specification in [`docs/plans/SMP_DESIGN.md`](docs/plans/SMP_DESIGN.md):

1. [**AP discovery and boot**](docs/roadmap/smp-piece1-ap-discovery.md) — Limine SMP protocol cross-checked with ACPI MADT; boots every core to a parked state.
2. [**Per-CPU storage**](docs/roadmap/smp-piece2-percpu.md) — GS-base CPU locals, per-CPU GDTs, TSS selectors, and IST1/IST2 stacks with SWAPGS and NMI isolation.
3. [**Lock discipline under real concurrency**](docs/roadmap/smp-piece3-lock-discipline.md) — Hierarchical rank checks (L1), contention telemetry, atomic spinlocks, and panic isolation.
4. [**SMP Scheduler & Work-Stealing**](docs/roadmap/smp-piece4-scheduler.md) — Per-CPU runqueues, 100 Hz APIC timer preemption, task migration, and dual-lock work-stealing across online cores.
5. [**IPIs and TLB shootdown**](docs/roadmap/smp-piece5-ipi.md) — APIC ICR messaging, synchronous broadcast TLB invalidation barriers, remote core wakeups, and real VMM unmap synchronization.
6. [**Multi-core memory architecture**](docs/roadmap/smp-piece6-memory.md) —
   - **6A**: 1 GiB boot ceiling and two-stage PMM/VMM initialization.
   - **6B**: PMM multi-core safety (320,000 cycles across 8 CPUs under 622k+ contention events, 0 duplicate claims, exact post-quiescence state equality).
   - **6C**: Contention-safe TLB shootdown with polled local servicing breaking circular deadlocks under interrupts-disabled contention, plus full TLB CR3 reload across all APs.
   - **6D**: Address-space lifetime discipline (`op_refs`, `sched_refs`, active CPU masks, deferred destruction queue, and atomic wait/exit coordination verified across 100 process cycles with zero leaks).

Next milestones:

- Network configuration: /bin/ifconfig (read-only), /bin/ifup + a config file
  (runtime IFSET), DNS server from the config file. DHCP deferred.
- Persistent rootfs integration (/paradise).
- MicroPython port.
- Accounts, permissions, and installer.

Larger follow-ups: a journaling filesystem (ext4 or similar), TLS, a DHCP
client, and a readiness primitive (select/poll) when a second consumer
appears.

See docs/roadmap/ for checkpoint history and hardware evidence, docs/plans/
for architectural plans, and AGENTS.md for implementation contracts and
invariants.

## Testing

The project combines host sanitizer tests, QEMU integration tests, offline filesystem checks, and manual hardware observations.

| Command | Coverage |
| --- | --- |
| `make test-smp-percpu` | BIOS/UEFI 1/4/8 CPUs: GS base, GDT/TSS, stack guards, NMI delivery on all CPUs |
| `make test-smp-append` | True multi-core SMP concurrent append verification under QEMU `-smp 4` (BIOS & UEFI): atomic serialization under `ext2_lock`, 200 records intact, 0 lost/corrupt, clean S5 shutdown, offline `e2fsck -fn` audit |
| `make test-shell-s6` | Shell S6 Phase 4A–4D under QEMU (BIOS & UEFI): child/parent redirection, dual-stream lexical ordering, stdin, stderr append/truncation/closure, expansion, failed setup and prompt recovery |
| `make test-shell-s6-resources` | Shell S6 resource exhaustion under BIOS & UEFI (1 and 4 CPUs): child descriptor limit, process table capacity, 0 uncollected threads, prompt recovery |
| `make test-vmm-host` | ASan/UBSan: VMM space registry, lifecycle states, transient `op_refs`, context switch tracking, deferred destruction queue with 0 leaks |
| `make test-pmm-boot-host` | ASan/UBSan: PMM boot ceiling, capped OOM, contiguous boundary, and unlock gates |
| `make test-smp-memory-boot` | BIOS/UEFI 1/4/8 CPUs: boot memory ceiling, readiness/CR3, and high-memory unlock |
| `make test-smp-vmm` | BIOS/UEFI 1/4/8 CPUs: 100 user process spawn/exit cycles across cores, deferred destruction, table frame and page leak checks |
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
| `make test-shell-host` | Consolidated ASan/UBSan: keyboard/queue, framebuffer CSI terminal parser, and line editor/search/history limits |
| `make test-shell-integration` | BIOS/UEFI shell interaction, cursor/screen-state, history/search/paste, and no-UART coverage |
| `make test-shell` | Shell interaction, input wakeups, process execution, and reclamation |
| `make test-nmi` | NMI injection at exact syscall transition boundaries in QEMU |
| `make test-boot-diagnostics` | UEFI boot with 8 GiB RAM and no COM1 |
| `make test-power` | QEMU shutdown and reboot commands |
| `make test-net-host` | ASan/UBSan: Phase 0 core abstractions, pbuf, RFC 1071 checksum, Ethernet II, ARP, and IPv4 codecs |
| `make test-net-eth-host` | ASan/UBSan: Ethernet/ARP stack with mocked NIC, scheduler, and ticks |
| `make test-net-eth` | QEMU BIOS/UEFI × e1000/e1000e × {user SLIRP, socket injection}: Ethernet/ARP delivery |
| `make test-net-rings` | QEMU BIOS/UEFI × e1000/e1000e, SMP=1: raw TX/RX ring regressions with exact 60-byte checks |
| `make test-net-i219-host` | ASan/UBSan: I219 SPT/CNP MAC takeover, DMA, and fatal containment |
| `make test-net-icmp-host` | ASan/UBSan: ICMP Echo codec, IPv4 validation, odd/even/zero payload, truncation, checksum corruption |
| `make test-net-ipv4-host` | ASan/UBSan: IPv4/ICMP stack with mocked callbacks |
| `make test-net-ping-host` | ASan/UBSan: ping mailbox and ABI adapters |
| `make test-net-icmp` | QEMU BIOS/UEFI × e1000/e1000e, SMP=1: `/bin/ping` integration, negative cases, timeout recovery |
| make test-net-udp-host | ASan/UBSan: UDP codec, pseudo-header checksum, odd/zero/max payload, corrupt/omitted checksum |
| make test-net-socket-host | ASan/UBSan: actual UDP socket/syscall code with memory/fd/tick/scheduler/NIC adapters |
| make test-net-udp | QEMU BIOS/UEFI × e1000/e1000e × user/socket, SMP=1: real /bin/udptest, pcap audit, 10/10 |
| make test-net-tcp-host | ASan/UBSan: TCP codec, options, checksums |
| make test-net-tcp-tcb-host | ASan/UBSan: TCP state machine with synthetic inputs |
| make test-net-tcp-socket-host | ASan/UBSan: TCP socket + syscall integration with mocked NIC/clock |
| make test-net-tcp | QEMU BIOS/UEFI × e1000/e1000e × user/socket, SMP=1: TCP client/server matrix with independent audit |
| make test-net-tcp-retention | Failure-artifact retention under assertion, timeout, and QEMU crash |
| make test-net-tcp-deadlines | QEMU BIOS/UEFI: SEND_UNTIL/RECV_UNTIL/CONNECT_UNTIL with real Ring 3 client |
| make test-net-dns-host | ASan/UBSan: DNS codec, literal RFC 1035 vectors, malformed-input fuzz, syscall adapters |
| make test-net-dns | QEMU BIOS/UEFI × e1000/e1000e × user/socket, SMP=1: nslookup, CNAME, NXDOMAIN, TC→TCP, stall timeout, hostname nc, 10/10 |
| make test-net-dns-lifecycle | QEMU BIOS/UEFI: SIGINT during timed TCP fallback, STOP/CONT past deadline, KILL recovery |

Recorded test results and their limits live in [`docs/roadmap/`](docs/roadmap/README.md). A listed test target is not a claim that every revision has passed it, and QEMU success is not physical-hardware acceptance. Storage tests must use disposable images, never an existing physical disk. Networking physical acceptance is recorded per phase; the Dell I219-LM path is the only NIC verified on bare metal.

## Finding your way around

```text
src/arch/x86_64/   CPU setup, SMP bring-up, APIC/IPI, interrupts, context switching, syscall entry
src/kernel/        Boot, scheduler, processes, ELF loader, syscalls, lock discipline
src/mm/            Physical memory (PMM), paging (VMM), heap
src/net/           Networking stack: Ethernet, ARP, IPv4, ICMP, UDP, TCP, DNS resolver, checksums, net_dev_t
src/drivers/       Console, input, PCI, NVMe, xHCI, USB BOT, e1000/e1000e/I219, power
src/fs/            VFS, tar initramfs, GPT, ext2, USB mount policy
src/include/       Shared kernel and user ABI definitions (syscalls, terminal)
user/              Freestanding user programs (init, hello, ping, udptest, echoc, echos, tcpserve, nc, nslookup, ps, sysinfo, top, nano) and shell entry
user/shell/        Modular shell engine (line editing, builtins, UI, RAM history, file editor)
tests/             Host tests and mocks
scripts/           Image creation and QEMU verification
docs/plans/        Architecture specifications and staged implementation plans
docs/roadmap/      Checkpoint implementation history and hardware verification evidence
```

Before changing kernel code, read [`PROTECTED.md`](PROTECTED.md) first, then [`AGENTS.md`](AGENTS.md). For supported formats, architectural limits, and technical debt, see [`ARCH_REVIEW.md`](ARCH_REVIEW.md). For design plans, see [`docs/plans/`](docs/plans/README.md). Contributions should state what changed, which tests were run, and whether the evidence comes from host tests, QEMU, or physical hardware.
