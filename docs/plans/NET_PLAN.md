# FortressOS Networking Milestone Plan — Architecture, Driver & Protocol Stack

Status: PLANNING ONLY (2026-09-30, Revision 2). No source changes, no tests. This document authorizes no kernel modifications.
Planning baseline: ground every claim in existing codebase contracts and file:line references.
Review criteria: critique by architecture reviewers prior to any implementation phase.

---

## 1. Context and Architectural Foundations

FortressOS currently possesses no networking subsystem. It is a freestanding C11/NASM x86_64 kernel (`AGENTS.md:21-23`) featuring:
- A cooperative/preemptive distributed scheduler with SMP work-stealing (`src/kernel/thread.c:580-629`).
- Strict lock ranking L1–L4 (`src/include/spinlock.h:34-59`, `AGENTS.md:220-226`, `PROTECTED.md:40-53`).
- Single-owner EOI and bounded IRQ handler rules I1–I3 (`src/arch/x86_64/idt.c:210-245`, `AGENTS.md:240-246`).
- Physical memory management capped at 1 GiB before kernel PML4 initialization, then unlocked to 32 GiB (`src/mm/pmm.h:9-17`, `src/kernel/main.c:4171`, `:4260`).
- Strict hardware DMA containment and quarantine on failure M4 (`src/drivers/xhci.c:616-620`, `src/drivers/nvme.c:13`, `AGENTS.md:254`, `PROTECTED.md:30-38`).
- A Ring 3 Unix-like user environment with process isolation and S1–S4 syscall ABI (`src/include/syscall_abi.h:1-45`, `src/kernel/syscall.c:1-50`).

Adding a network interface card (NIC) driver and an Internet protocol stack (Ethernet, ARP, IPv4, ICMP, UDP) represents the largest capability jump in FortressOS. Because networking interacts directly with PCI bus mastering, physical DMA rings, memory management, and blocking syscall I/O, it directly touches five protected architectural contracts:
1. **DMA Ownership & Quarantine (M4, §9)**: Controller DMA buffers must never be freed while hardware can access them.
2. **Interrupt & Ingress Discipline (I1, I2, §9)**: Ingress must not violate IRQ rules. For Milestone NET-1, all ingress is **polling-only**, eliminating IRQ complexity.
3. **Lock Hierarchy (L1, `spinlock.h`)**: Network locks sit at Rank 1. Rank-1 locks **never nest**; operations use strict copy-out/drop/acquire sequencing.
4. **Subsystem Init Ordering in `kmain` (§9)**: NIC MMIO and DMA setup must occur after the kernel PML4 is loaded and high memory is unlocked.
5. **No Ad-Hoc Sleep APIs (S3c, §4)**: Socket blocking I/O must strictly use `sched_wait_until()` with a predicate, never inventing a `wait_queue_sleep` primitive (`AGENTS.md:236`).

This plan specifies the hardware targets, layered protocol design, DMA/lock discipline, initialization sequence, user-facing surface, incremental phasing, reviewer push-back resolutions, failure handling, and explicit non-goals.

---

## 2. Hardware Target Evaluation & Decision

### 2.1 Hardware Candidates

Two candidate families exist for initial implementation:

1. **VirtIO-Net (`virtio-net-pci`, Device ID `0x1000` / `0x1041`)**:
   - *Architecture*: Split/packed virtqueue rings (descriptor table, available ring, used ring) with VirtIO PCI transport.
   - *Pros*: Standard QEMU paravirtualized device, highly efficient in virtualization.
   - *Cons*: **Virtual hardware only**. It cannot run on bare-metal PC hardware. Implementing VirtIO-Net provides zero code reuse for the Dell laptops; reaching physical hardware would require writing a second, completely distinct NIC driver from scratch.

2. **Intel Gigabit Ethernet Family (`e1000` / `e1000e` / I219-LM)**:
   - *Architecture*: Circular descriptor rings (16-byte legacy descriptors for TX and RX), MMIO register control (BAR0, 128 KiB or 512 KiB aperture), direct packet buffer DMA.
   - *QEMU support*: `-device e1000` (Intel 82540EM, PCI `8086:100E`) and `-device e1000e` (Intel 82574L, PCI `8086:10D3`).
   - *Dell Hardware*:
     - **Dell Latitude 5590**: Intel Core i5-8350U (Sunrise Point-LP / Kaby Lake-R PCH). Integrated NIC: **Intel Ethernet Connection I219-LM** (PCI Vendor `0x8086`, Device ID `0x15D7`).
     - **Dell Latitude 5500**: Intel 8th-Gen Core (Cannon Point-LP / Whiskey Lake PCH). Integrated NIC: **Intel Ethernet Connection I219-LM** (PCI Vendor `0x8086`, Device ID `0x15BD` or `0x15BB`).
   - *Pros*:
     - The Intel 82540EM (`e1000`), 82574L (`e1000e`), and I219-LM share the **identical fundamental ring layout**:
       - Identical 16-byte `rx_desc` (uint64_t buffer_addr, uint16_t length, uint16_t csum, uint8_t status, uint8_t errors, uint16_t special).
       - Identical 16-byte `tx_desc` (uint64_t buffer_addr, uint16_t length, uint8_t cso, uint8_t cmd, uint8_t status, uint8_t css, uint16_t special).
       - Identical circular head/tail register pointers: `RDBAL`/`RDBAH`, `RDLEN`, `RDH`/`RDT`, `TDBAL`/`TDBAH`, `TDLEN`, `TDH`/`TDT`.
       - Identical Receive Address Low/High filter registers (`RAL0`/`RAH0`).
     - A driver written for Intel Gigabit Ethernet in QEMU is **directly portable** to bare-metal Dell hardware with minimal PHY/clock adjustments.

### 2.2 Decision & Justification

**Decision**: Target the **Intel Gigabit Ethernet family directly**, supported by a clean driver-to-stack abstraction (`net_dev_t`).

**Justification**:
- Writing VirtIO-Net first introduces a disposable driver that cannot run on the project's primary hardware verification target (Dell Latitude 5590/5500).
- Writing the Intel driver in QEMU (`-device e1000` or `-device e1000e`) exercises the exact same descriptor structures, MMIO mechanics, and DMA memory layouts that the Dell I219-LM controller requires.
- By defining a modular `net_dev_t` interface (mirroring `block_dev_t` in `src/drivers/block.h:9-17`), the protocol stack (Ethernet, ARP, IPv4, ICMP, UDP) is completely independent of the underlying controller. If VirtIO-Net or a Realtek driver is ever desired in the future, it can implement `net_dev_t` without touching a single line of the protocol stack.

---

## 3. Subsystem Architecture & Layered Scope (Milestone NET-1)

Milestone NET-1 covers the foundational stack through UDP and Dell bare-metal acceptance. TCP is strictly deferred to Milestone NET-2.

```
+-------------------------------------------------------------------+
|               Ring 3 User Programs (/bin/ping, /bin/udptest)      |
+-------------------------------------------------------------------+
                                  |
                                  | Syscall ABI (SYS_SOCKET, SYS_SENDTO, SYS_RECVFROM)
                                  v
+-------------------------------------------------------------------+
|               Transport Layer: UDP (Milestone NET-1)              |
+-------------------------------------------------------------------+
                                  |
                                  v
+-------------------------------------------------------------------+
|               Network Layer: IPv4, ICMP (Ping), ARP               |
+-------------------------------------------------------------------+
                                  |
                                  v
+-------------------------------------------------------------------+
|               Data Link Layer: Ethernet Framing & MAC             |
+-------------------------------------------------------------------+
                                  |
                                  v
+-------------------------------------------------------------------+
|               Network Device Interface (net_dev_t)                |
+-------------------------------------------------------------------+
                                  |
                                  v
+-------------------------------------------------------------------+
|       Hardware Driver: Intel e1000 / e1000e / I219-LM (Polling)   |
+-------------------------------------------------------------------+
```

### 3.1 Packet Buffer Structure (`pbuf_t`)
Network data packets are managed using a bounded, pre-allocated frame structure:
```c
#define PBUF_CAPACITY 2048

typedef struct pbuf {
    struct pbuf *next;          /* Intrusive queue link */
    uint8_t     *payload;       /* Pointer into data buffer (advances past headers) */
    uint16_t     length;        /* Current layer payload length */
    uint16_t     total_len;     /* Total packet length */
    uint32_t     flags;         /* PBUF_DMA_BACKED, etc. */
    uint8_t      data[PBUF_CAPACITY]; /* Physical packet buffer (aligned) */
} pbuf_t;
```

**Ownership Lifecycle Across Layers**:
1. **NIC RX Ring**: Pre-allocated `pbuf_t` buffers are registered into RX descriptors (`buffer_addr = virt_to_phys(pbuf->data)`).
2. **Worker Claim**: When the worker polls the RX ring and observes the `DD` (Descriptor Done) bit, it claims the filled `pbuf_t` and inserts a fresh pre-allocated `pbuf_t` into the descriptor, advancing `RDT`.
3. **Stack Demux**: The worker walks Ethernet -> IPv4 -> UDP headers without copying, advancing `pbuf->payload` and decrementing `pbuf->length`.
4. **Socket Ingress**: The `pbuf_t` is linked into the target socket's `rx_queue`.
   - *Full-Queue Behavior*: Each socket has a bounded queue capacity of 16 packets. If full, the incoming `pbuf_t` is dropped immediately, `sock->rx_dropped` is incremented, and the `pbuf_t` is recycled. Memory is never leaked and producers never block.
5. **User Copy**: In `sys_recvfrom()`, payload data is copied to the validated user buffer (`vmm_validate_user_range`). Once consumed, the `pbuf_t` is returned to the free pool.

### 3.2 Ingress Worker Thread
In Milestone NET-1, all packet ingress is **polling-only** (zero interrupt handlers).
- **Creation & BSP Pinning**: Spawned once in `net_boot_probe()` during kernel initialization via `thread_create_on_cpu(0, "net_worker", net_worker_main, NULL)` (`src/kernel/thread.h:106`). Pinning strictly to the BSP ensures RX-ring polling stays on one core, mirroring the input/terminal worker's single-core discipline (`AGENTS.md:27-28`, `src/drivers/input.c`); shared device access is still serialized by `g_net_dev_lock`.
- **Priority**: Standard kernel thread priority.
- **Cadence & Sleep Mechanism**: Runs an iterative polling loop. Under `g_net_dev_lock`, it claims completed packets from the RX ring into a local batch list. If packets were processed, it yields to allow consumers to run (`thread_yield()`). If the ring is idle (no packets received), the worker executes a deadline-bounded `sched_wait_until(&g_net_poll_channel, net_poll_deadline_reached, &deadline)` where `deadline = apic_timer_get_bsp_ticks() + 1`. This is woken periodically by the timer tick (`apic_timer`), precisely matching the tick-wake mechanism the terminal ingress worker relies on (`src/drivers/input.c:239`), ensuring it never spins at 100% CPU nor sleeps on an unserviced channel without a producer.
- **Lock Discipline**: Completely separates device polling from socket queueing (see Section 4.3).

---

## 4. Protected Contracts: DMA, Lock Ranks, and Blocking

### 4.1 DMA Ownership, Quiescence, and Quarantine (Contract M4, §4 & §9)

The kernel storage subsystems (NVMe and xHCI) establish strict DMA contracts:
- `AGENTS.md:254` (M4): *"Never free DMA memory while a controller may still use it."*
- `PROTECTED.md:30-31`: *"`ENABLE_*` raw-write gates, disposable fixture separation, hardware storage exclusions, and DMA quarantine."*

The network driver strictly adheres to this discipline:
1. **Allocation & Virtual Mapping**:
   - All descriptor rings and packet buffers are allocated from PMM (`pmm_alloc_page()`, `src/mm/pmm.h:21`).
   - Virtual addresses must be derived via `vmm_phys_to_virt(phys)` (`src/mm/vmm.h:103`), never by casting raw physical integers (Contract M1, `AGENTS.md:251`).
2. **Two-Stage PMM/VMM Timing**:
   - **Zero NIC DMA buffers or rings may be allocated during early `kmain`** while PMM is capped at 1 GiB (`PMM_BOOT_ALLOC_LIMIT`, `src/mm/pmm.h:9`).
   - All network memory allocation occurs strictly post-`pmm_unlock_high_memory()` (`src/kernel/main.c:4260`), when the kernel PML4 is loaded and covers all managed RAM up to 32 GiB.
   - Intel e1000/e1000e/I219-LM possesses 64-bit base registers (`TDBAH`/`RDBAH`), allowing full 64-bit DMA.
3. **Quiescence Procedure**:
   - Before resetting the NIC or unregistering, the driver disables RX and TX engines (`RCTL.EN = 0`, `TCTL.EN = 0`), clears interrupt masks (`IMC = 0xFFFFFFFF`), disables PCI Bus Mastering (`PCI_COMMAND_BUS_MASTER` cleared, `src/drivers/pci.h:34-37`), and executes a bounded poll loop (up to 10 ms via `delay_ms()`, `src/drivers/xhci.c:48-63`).
4. **Quarantine on Failure**:
   - If controller reset or quiescence times out:
     - The driver sets `g_net_fatal = true`.
     - Bus mastering remains permanently disabled.
     - **All physical DMA frames (rings and packet buffers) are permanently quarantined**: they are marked unfreeable and NEVER returned to PMM (`src/drivers/xhci.c:616-620`, `src/drivers/nvme.c:13`).

### 4.2 Ingress Discipline: Polling-Only in NET-1 (Contracts I1, I2, §4 & §9)

- `AGENTS.md:243` (I1) and `AGENTS.md:244` (I2): Regulate hardware interrupt handlers.
- **NET-1 Policy**: In Milestone NET-1, **no hardware interrupt handlers or MSI vectors are registered**.
- Ingress is entirely driven by the dedicated `net_worker` kernel thread polling the RX descriptor ring (`net_poll_rx()`).
- This completely avoids unverified ACPI `_PRT` routing, stale `PCI_REG_INTERRUPT_LINE` values on UEFI firmware, and LAPIC EOI races.
- Interrupt-driven ingress (MSI capability parsing + vector allocation) is deferred to a future milestone.

### 4.3 Lock Hierarchy: Strictly Resolving Rank-1 Nesting (Contract L1, `spinlock.h:34-59`)

The binding lock hierarchy in `spinlock.h:35-39` specifies:
```
Level 1: per-CPU scheduler locks OR ext2_lock OR g_process_lock
Level 2: g_heap_lock    (Kernel heap & free list)
Level 3: g_vmm_lock     (Page tables & virtual mapping)
Level 4: g_pmm_lock     (Physical frame bitmap allocator)
Level 5: g_console_lock (Leaf output lock)
```
*Rule*: Rank-1 locks **never nest** (`spinlock.h:40-42`).

Network synchronization uses two Rank-1 spinlocks:
- `g_net_dev_lock`: `SPINLOCK_RANKED(1, "net_dev")` — protects NIC hardware rings and device state.
- `g_socket_table_lock`: `SPINLOCK_RANKED(1, "socket_table")` — protects the socket registry and socket queues.

#### Proving Non-Nesting via Call Graphs:

**Case 1: Outbound Transmission (`sys_sendto`)**:
```
sys_sendto()
  │
  ├─► [1] Acquire g_socket_table_lock
  │       Lookup socket by fd; validate connected/dest address.
  │       Copy payload from user space into local pbuf_t.
  ├─► [2] Release g_socket_table_lock  ◄── MUST DROP BEFORE TX
  │
  └─► [3] Call net_send_packet(dev, pbuf)
            │
            ├─► Acquire g_net_dev_lock
            │   Check TX ring capacity; write tx_desc; update TDT.
            └─► Release g_net_dev_lock
```
*Proof*: `g_socket_table_lock` is released at Step 2 before `g_net_dev_lock` is acquired at Step 3. Zero concurrent locks held.

**Case 2: Ingress Worker Demux (`net_worker_main`)**:
```
net_worker_main()
  │
  ├─► [1] Acquire g_net_dev_lock
  │       Check RX descriptors (DD bit); detach filled pbuf_t list to local batch.
  │       Advance RDT.
  ├─► [2] Release g_net_dev_lock       ◄── MUST DROP BEFORE DEMUX
  │
  └─► [3] For each pbuf in local batch:
            Parse Ethernet / IP / UDP headers in thread context (no locks).
            │
            ├─► [3a] Acquire g_socket_table_lock
            │        Find socket by dest port; enqueue pbuf (or drop if full).
            ├─► [3b] Release g_socket_table_lock ◄── MUST DROP BEFORE WAKE
            │
            └─► [3c] sched_wake_all(&sock->wait_channel)
```
*Proof*: `g_net_dev_lock` is dropped at Step 2 before socket demux begins. `g_socket_table_lock` is dropped at Step 3b before waking the wait channel. Zero concurrent locks held.

### 4.4 Socket Blocking Read (`recvfrom`): The Race-Free Sleep Pattern (Contracts S3b & S3c)

- `AGENTS.md:235` (S3b): *"Sleep releases all locks before switching. Wake rechecks predicate."*
- `AGENTS.md:236` (S3c): *"Do not invent a `wait_queue_sleep` API — see `input_read` / `sched_wait_until`."*

`sys_recvfrom()` follows the exact `input_read()` pattern (`src/drivers/input.c`, `src/kernel/thread.c:633-670`):

```c
static bool socket_has_data(void *arg) {
    socket_t *sock = (socket_t *)arg;
    return sock->rx_queue != NULL || sock->state == SOCK_ERROR;
}

int64_t sys_recvfrom(...) {
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&g_socket_table_lock);
        if (sock->rx_queue != NULL) {
            pbuf_t *pbuf = dequeue_pbuf(&sock->rx_queue);
            spin_unlock_irqrestore(&g_socket_table_lock, flags);
            /* Copy out to user buffer... */
            return bytes_copied;
        }
        if (sock->state == SOCK_ERROR) {
            spin_unlock_irqrestore(&g_socket_table_lock, flags);
            return -ENETDOWN;
        }
        spin_unlock_irqrestore(&g_socket_table_lock, flags);

        /* S3c: Use sched_wait_until, releasing all locks before sleep */
        sched_wait_until(&sock->wait_channel, socket_has_data, sock);
    }
}
```

**Walkthrough of the Lost-Wakeup Race**:
- *The Trap*: If Thread T checked `sock->rx_queue == NULL`, dropped the lock, and then called an ad-hoc sleep, a packet could arrive on `net_worker`, enqueue, call `wake()`, and find no blocked threads — causing T to sleep indefinitely.
- *The Defense in `sched_wait_until()`*:
  1. `sched_wait_until` acquires `g_sched_lock` (`src/kernel/thread.c:638`).
  2. With scheduler interrupts disabled, it executes `ready(arg)` (`socket_has_data`).
  3. If a packet arrived just before entering `sched_wait_until()`, `ready()` evaluates to `true`, and it returns immediately without sleeping.
  4. If not ready, it atomically inserts the thread into `g_blocked_threads` on `&sock->wait_channel` and performs context switch (`src/kernel/thread.c:650-653`).
  5. The producer (`net_worker`) enqueues the packet *before* calling `sched_wake_all(&sock->wait_channel)`. Because `sched_wake_all()` takes `g_sched_lock`, it is strictly serialized with the state transition. Lost wakeups are structurally impossible.

---

## 5. Subsystem Initialization & IP Configuration via Boot Cmdline

### 5.1 Subsystem Initialization in `kmain`
Following `src/kernel/main.c:4032-5676`, networking initializes after `vmm_init()` (line 4260), after `ioapic_init()` (line 5001), and after SMP bring-up (line 5602):
```c
/* Post-SMP, post-storage probe in kmain (main.c:5636): */
boot_status("Probing network controllers...");
pci_report_nic();               /* Log discovered NICs */
net_boot_probe(&boot_info);     /* Initialize e1000/I219, rings, worker thread, and IP config */
```

### 5.2 IP Configuration via Boot Command Line
To enable static configuration on bare-metal hardware without hardcoding IP addresses, networking reuses the bounded boot command-line parsing discipline established by USB storage ([`docs/subsystems/usb.md`](../subsystems/usb.md#explicit-usb-selection-and-writable-opt-in), `src/fs/usb_mount.c:30-70`).

**Parameter Format**:
```
net=<ip>/<prefix>,<gateway>
```
*Examples*:
- QEMU default: `net=10.0.2.15/24,10.0.2.2`
- Dell bare-metal on local LAN: `net=192.168.1.150/24,192.168.1.1`

**Parsing Rules**:
- Handled during `net_boot_probe()` from `boot_info->cmdline`.
- Strictly bounded: parses dot-decimal IPv4 octets, prefix length (1–30), and gateway IP.
- If parameter is missing: defaults to QEMU guest configuration (`10.0.2.15/24`, gateway `10.0.2.2`).
- Malformed parameters log a warning and fall back to defaults without panicking.

---

## 6. User-Facing Surface & Syscall ABI

### 6.1 Syscall Numbers & ABI

Existing syscall numbers end at `SYS_SYSINFO = 37` (`src/include/syscall_abi.h:45`). Networking syscalls begin at 38:

| Number | Macro | Signature | Description |
| --- | --- | --- | --- |
| 38 | `SYS_SOCKET` | `(int domain, int type, int protocol) -> int fd` | Allocate socket descriptor |
| 39 | `SYS_BIND` | `(int fd, const struct sockaddr *addr, uint32_t addrlen) -> int` | Bind socket to local port |
| 40 | `SYS_SENDTO` | `(int fd, const void *buf, size_t len, int flags, const struct sockaddr *dest, uint32_t addrlen) -> int64_t` | Send packet to destination |
| 41 | `SYS_RECVFROM` | `(int fd, void *buf, size_t len, int flags, struct sockaddr *src, uint32_t *addrlen) -> int64_t` | Receive packet with sender info |
| 42 | `SYS_NETCTL` | `(int cmd, void *arg, size_t arg_size) -> int` | Interface info, IP/gateway config, link status |

### 6.2 Contract S1 & S2 Compliance
- **S1 (ABI registers)**: Arguments passed in RDI, RSI, RDX, R10, R8, R9. Results returned in RAX (`src/kernel/syscall.c:1-30`).
- **S2 (Range Validation)**: Every user pointer is validated via `vmm_validate_user_range(pml4, ptr, size, write_req)` (`src/mm/vmm.h:104`).
- **`SYS_RECVFROM` `addrlen` S2 Contract**:
  - `uint32_t *addrlen` is a read/write pointer:
    1. Validate range: `vmm_validate_user_range(pml4, addrlen, sizeof(uint32_t), true)`.
    2. Read caller's buffer capacity: `uint32_t cap = *addrlen;`
    3. Validate `src` destination buffer: `vmm_validate_user_range(pml4, src, cap, true)`.
    4. Copy sender `sockaddr_in` up to `min(cap, sizeof(struct sockaddr_in))`.
    5. Write back actual size: `*addrlen = sizeof(struct sockaddr_in);`.

### 6.3 Standalone Ring 3 Utilities
1. `/bin/ifconfig`: Displays interface name, MAC address, IPv4 address, subnet mask, gateway, and TX/RX statistics.
2. `/bin/ping <ip>`: Sends ICMP Echo Requests once per second, prints RTT in milliseconds via APIC timer ticks, and summarizes statistics on exit.
3. `/bin/udptest <ip> <port> <message>`: Minimal Ring 3 UDP client for test verification.

---

## 7. Failure Handling & Stop-Conditions

### 7.1 Link-Down / Interface-Down Failure Story
The network status register `E1000_REG_STATUS` bit `LU` (Link Up) is periodically sampled by `net_worker`:
- **Outbound Transmission (`sys_sendto`)**: If `STATUS.LU == 0`, transmission fails immediately and returns `-ENETDOWN`.
- **Blocked Receive (`sys_recvfrom`)**: If link goes down while a thread is blocked, `net_worker` transitions the socket state to `SOCK_ERROR` and wakes all waiters on `&sock->wait_channel`. Blocked calls awaken and return `-ENETDOWN`.
- **In-Flight ARP**: ARP resolution is bounded by 3 attempts at 1000 ms intervals (using APIC timer ticks). If no reply is received, `arp_resolve()` fails and returns `-EHOSTUNREACH`. Packets are never queued indefinitely.

### 7.2 Phase 2b Stop-Condition (Dell Hardware Bring-Up)
Intel I219-LM on Sunrise Point / Cannon Point platforms can experience PHY clock gating or CSME power holds.
Following the strict **Phase 9G.1 stop-condition discipline** ([`docs/subsystems/usb.md`](../subsystems/usb.md#9g1-checkpoints-and-debugging)):
- If `STATUS.LU == 0` after bounded MDIC PHY reset attempts (capped at 500 ms):
  1. Capture an immutable diagnostic record of controller registers (`STATUS`, `CTRL`, `MDIC`, `EXTCNF_CTRL`).
  2. Log the snapshot to COM1 and framebuffer screen.
  3. **STOP execution of network bring-up**. Do not thrash or guess blind workarounds.
  4. Disable PCI Bus Mastering and leave the controller safely stopped.
  5. The kernel proceeds to the shell prompt with networking cleanly marked unavailable.

---

## 8. Phasing & Milestone Breakdown (Milestone NET-1)

| Phase | Title | Primary Deliverable | Acceptance Gate |
| --- | --- | --- | --- |
| **Phase 0** | Core Abstractions & Buffers | `net_dev_t`, `pbuf_t`, RFC 1071 checksums, header parsers | Host ASan/UBSan unit tests (`make test-net-host`) |
| **Phase 1a** | PCI Discovery & MMIO (QEMU) | Detect Intel NIC, map BAR0 aperture, read MAC address | QEMU BIOS/UEFI boot log displays NIC model & MAC |
| **Phase 1b** | Dell Hardware Discovery | Probing Dell Latitude 5590 / 5500 on physical hardware | Physical boot log confirms I219-LM BAR0 mapping & MAC |
| **Phase 2a** | Rings & Raw Frame I/O (QEMU) | Allocate TX/RX rings, send raw frame, poll RX frame | QEMU `-netdev dump` pcap audit of transmitted frame |
| **Phase 2b** | Dell Physical Link & Raw Frame | Link-up check (`STATUS.LU == 1`) and raw frame send on Dell | Physical link-up confirmed; Phase 2b stop-condition enforced |
| **Phase 3** | Ethernet & ARP | 14-byte Ethernet framing, ARP cache, Request/Reply | QEMU gateway ARP resolution verified |
| **Phase 4a** | IPv4 & ICMP Ping (QEMU) | IPv4 parser/checksum, ICMP Echo Reply, `/bin/ping` | Host pings QEMU guest; `/bin/ping 10.0.2.2` succeeds |
| **Phase 4b** | Dell Physical Ping Acceptance | Physical cable ping from Dell to local network gateway | Physical ping exchange verified on Dell Latitude hardware |
| **Phase 5** | UDP & Socket Syscalls | UDP protocol, socket table, `SYS_SOCKET`/`SENDTO`/`RECVFROM` | `/bin/udptest` verified in QEMU and Dell — **Closes Milestone NET-1** |

---

## 9. Explicit Non-Goals ("What Networking Does NOT Do in NET-1")

To prevent scope creep and maintain architectural boundaries, the following features are explicitly **out of scope for Milestone NET-1**:

- **No Interrupts or MSI in NET-1**: Polling-only ingress via `net_worker`. MSI deferred to future driver milestone.
- **No TCP in Milestone NET-1**: Minimal client TCP streaming is deferred to Milestone NET-2.
- **No IPv6**: IPv4 only.
- **No Wireless / Wi-Fi (802.11)**: Wired Ethernet (802.3) only.
- **No In-Kernel TLS / HTTPS**: No cryptographic handshakes in Ring 0.
- **No Dynamic DHCP Client in NET-1**: Fixed static IP configuration via `net=<ip>/<prefix>,<gateway>` boot parameter.
- **No Hardware Offloads**: No TSO, LRO, or hardware checksum offload.
- **No Multiple Active NICs or Bridging/Bonding**: Single primary interface (`eth0`).
- **No Hot-Plug NIC Enumeration**: Controller must be present at boot.

### 9.1 I219 PHY/CSME and reset risk (Phase 2b)

The original §9.1 citation was missing; the risk was documented in §7.2.
I219 is a PHY attached to the integrated PCH MAC. Shared legacy descriptors
do not establish shared initialization/reset behavior. Intel's
[I219 datasheet](https://cdrdv2-public.intel.com/612523/ethernet-connection-i219-datasheet.pdf)
and [e1000e PCH reference](https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/intel/e1000e/ich8lan.c)
document firmware ownership, PHY state and PCH-specific setup. The
[I219 descriptor-flush reference](https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/intel/e1000e/netdev.c)
also warns of a unit hang if reset occurs with descriptors requiring flush.

The bounded first Phase 2b implementation takes over an already negotiated
PHY with a MAC-only reset; it does not force PHY reset, power-cycle the PHY or
override CSME/SMBus configuration. A failed 500 ms link check, ownership wait,
transaction drain or reset stops networking with immutable diagnostics and
retained DMA. A descriptor-flush requirement stops without reset. This is a
deliberate narrower implementation than §7.2's optional MDIC reset attempts;
full PHY recovery requires a separately documented post-PHY configuration path.

The physical Phase 2b gate is a matching `0x88B5` raw frame captured on a wired
second machine on the same broadcast domain, together with Dell TX DD and link
evidence. DD plus link-up is software evidence only: without a capture, record
"TX completed, wire not observed" and leave physical acceptance pending.
Implementation and evidence boundaries: [Phase 2b](../roadmap/net-phase2b.md).

Remaining reference-init differences reviewed for the RX-first experiment:

| Area | FortressOS vs Linux v6.12 e1000e | Next action |
| --- | --- | --- |
| PHY/D0/ULP | Inherit negotiated PHY; Linux performs PHY ownership, D0/ULP and copper-link setup | RX result first; a PHY recovery path needs bounded MDIC/page/ownership handling, not a guessed FWSM write |
| PBA | Dell inherited RX18/TX14 KiB; SPT reference requests RX26 KiB | Difference recorded; insufficient capacity for the 60-byte test is not established |
| GCR | Managed no-snoop bits clear; Linux helper also ORs upper bits | Full-register difference remains unqualified; no speculative upper-bit write |
| IOSFPC/TARC/ECC | SPT workaround setup already implemented | Preserve for the isolated RX experiment |
| FWSM | FW_VALID=1, WLOCK_MAC=4, raw MODE=6 | Firmware status; no DMA clock-enable bit to program here; respect firmware ownership |

The next boot uses `net_test=rings net_rx_first=1`, then decodes FWSM.
It adds no init writes, so the experiment can separate receive progress from
the TX failure without changing the suspected datapath. A missing peer frame
alone is not proof of unreachable DMA backing. Physical acceptance stays open.

Following physical RX descriptor completion evidence, the 15D7 path now
implements the reference PLL/K1/FIFO-gap link-up subset, MAC beacon duration
and SPT K1-off propagation. Bounded MDIC and page/firmware ownership cleanup
are described in the Phase 2b roadmap. This does not implement a full PHY
reset, D0/ULP recovery, EEE or platform LTR setup. The next build switches to
`net_test=rings net_tx_trial=1` for an isolated TX attempt; physical TX remains
unverified. Containment, descriptors, TXDCTL and FEXTNVM11 are unchanged.
