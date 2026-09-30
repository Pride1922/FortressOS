# FortressOS Networking Milestone Plan — Architecture, Driver & Protocol Stack

Status: PLANNING ONLY (2026-09-30). No source changes, no tests. This document authorizes no kernel modifications.
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

Adding a network interface card (NIC) driver and an Internet protocol stack (Ethernet, ARP, IPv4, ICMP, UDP, TCP) represents the largest capability jump in FortressOS. Because networking interacts directly with PCI bus mastering, physical DMA rings, asynchronous ingress interrupts, kernel memory allocation, and blocking syscall I/O, it directly touches five protected architectural contracts:
1. **DMA Ownership & Quarantine (M4, §9)**: Controller DMA buffers must never be freed while hardware can access them.
2. **Interrupt & Ingress Discipline (I1, I2, §9)**: Ingress ISRs must be bounded, non-allocating, non-blocking, and single-EOI compliant.
3. **Lock Hierarchy (L1, `spinlock.h`)**: Network locks must sit at an unambiguous rank without illegal nesting.
4. **Subsystem Init Ordering in `kmain` (§9)**: NIC MMIO and DMA setup must occur after the kernel PML4 is loaded and high memory is unlocked.
5. **No Ad-Hoc Sleep APIs (S3c, §4)**: Socket blocking I/O must strictly use `sched_wait_until()` with a predicate, never inventing a `wait_queue_sleep` primitive (`AGENTS.md:236`).

This plan specifies the hardware targets, layered protocol design, DMA/IRQ discipline, lock hierarchy, initialization sequence, user-facing surface, incremental phasing, reviewer push-back points, risks, and explicit non-goals.

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
- By defining a modular `net_dev_t` interface (mirroring `block_dev_t` in `src/drivers/block.h:9-17`), the protocol stack (Ethernet, ARP, IPv4, ICMP, UDP, TCP) is completely independent of the underlying controller. If VirtIO-Net or a Realtek driver is ever desired in the future, it can implement `net_dev_t` without touching a single line of the protocol stack.

---

## 3. Subsystem Architecture & Layered Scope

The networking subsystem is organized into strictly bounded layers. Each layer has an explicit deliverable, verification test, and acceptance gate.

```
+-------------------------------------------------------------------+
|               Ring 3 User Programs (/bin/ping, /bin/netstat)      |
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
|     Hardware Driver: Intel e1000 / e1000e / I219-LM (PCI MMIO)    |
+-------------------------------------------------------------------+
```

### Layer 0: PCI Discovery & MMIO Mapping
- **Scope**:
  - Scan PCI bus (`src/drivers/pci.c:150-153`) for `class_code == PCI_CLASS_NETWORK` (`0x02`, `src/drivers/pci.h:48`), `subclass == 0x00` (Ethernet).
  - Match Vendor `0x8086` and Device IDs:
    - `0x100E` (QEMU 82540EM `e1000`)
    - `0x10D3` (QEMU 82574L `e1000e`)
    - `0x15D7` / `0x15BD` / `0x15BB` (Dell Latitude 5590 / 5500 Intel I219-LM)
  - Verify and decode BAR0: ensure 32-bit or 64-bit non-prefetchable memory aperture (`src/drivers/pci.h:78-85`).
  - Map MMIO aperture into dedicated kernel virtual memory (`0xFFFFFFFFE2000000ULL`) via `vmm_map_page()` with flags `PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT | PTE_NX` (`src/mm/vmm.h:8-17`).
  - Read factory MAC address from Receive Address Registers (`RAL0`/`RAH0`) or EEPROM.
- **Acceptance Gate**: Boot log prints detected NIC model, PCI BDF, MMIO physical base, virtual mapping, and hardware MAC address (`XX:XX:XX:XX:XX:XX`). No DMA memory allocated yet.

### Layer 1: Hardware Driver — Rings, DMA, Completion Engine
- **Scope**:
  - Allocate circular descriptor rings from PMM (`pmm_alloc_page()`, `src/mm/pmm.h:21`):
    - Transmit (TX) ring: 64 descriptors × 16 bytes = 1024 bytes (1 page).
    - Receive (RX) ring: 64 descriptors × 16 bytes = 1024 bytes (1 page).
  - Allocate packet frame buffers: 64 RX buffers (2048 bytes each, 32 pages) and TX bounce buffers.
  - Program NIC registers: `TDBAL`/`TDBAH`, `TDLEN`, `TDH`/`TDT`, `TCTL`; `RDBAL`/`RDBAH`, `RDLEN`, `RDH`/`RDT`, `RCTL`.
  - Enable PCI Bus Mastering (`PCI_COMMAND_BUS_MASTER`, `src/drivers/pci.h:36`).
  - **Completion Engine**:
    - *Transmit (TX)*: Synchronous circular producer with bounded poll on descriptor `DD` (Descriptor Done) bit. If ring is full, caller spins with bounded timeout or yields.
    - *Receive (RX)*: Ingress is asynchronous. Two operating modes:
      1. *Polling mode* (Phase 1–2): `net_poll_rx()` checks descriptor `DD` bit, copies payload, advances `RDT`. Follows xHCI's bounded polling discipline (`src/drivers/xhci_bot.c:445-450`).
      2. *Interrupt mode* (Phase 3+): Device triggers IRQ on packet arrival; ISR signals a deferred ingress thread (see Section 4).
- **Acceptance Gate**: Raw packet loopback or host send/receive test. Driver transmits a raw 64-byte Ethernet frame captured by QEMU `-netdev dump`, and successfully receives an incoming raw frame.

### Layer 2: Network Device Abstraction & Ethernet Framing
- **Scope**:
  - Define `net_dev_t` in `src/include/net.h`:
    ```c
    typedef struct net_dev {
        char        name[16];           /* "eth0" */
        uint8_t     mac_addr[6];
        uint32_t    mtu;                /* 1500 */
        uint32_t    flags;              /* NET_UP, NET_RUNNING */
        int       (*send_packet)(struct net_dev *dev, const void *buf, size_t len);
        int       (*poll_rx)(struct net_dev *dev);
        void       *priv;
    } net_dev_t;
    ```
  - Define 14-byte Ethernet frame header (`eth_header_t`): Destination MAC (6B), Source MAC (6B), EtherType (2B).
  - Validate minimum frame size (60 bytes excluding 4-byte FCS) and MTU (1514 bytes maximum frame length).
  - EtherType dispatch: `0x0806` (ARP), `0x0800` (IPv4). Discard unsupported EtherTypes.
  - Frame filtering: accept unicast to `net_dev.mac_addr` and broadcast (`FF:FF:FF:FF:FF:FF`); drop other unicast frames.
- **Acceptance Gate**: Host unit tests (`tests/net_eth_host.c`) with sanitizer coverage; verifies header encoding, decoding, length validation, broadcast recognition, and runt-frame rejection.

### Layer 3: ARP (Address Resolution Protocol, RFC 826)
- **Scope**:
  - ARP packet structure: Hardware type (Ethernet=1), Protocol type (IPv4=0x0800), Hardware size (6), Protocol size (4), Opcode (Request=1, Reply=2), Sender MAC/IP, Target MAC/IP.
  - Static/Dynamic ARP Cache: bounded array of 16 entries with state (`FREE`, `RESOLVING`, `RESOLVED`), IP address, MAC address, and timer ticks.
  - Ingress handling:
    - On ARP Request for our IP: construct and send unicast ARP Reply to sender MAC.
    - On ARP Reply: update ARP cache entry and wake any pending outbound transmit waiters.
  - Outbound resolution: `arp_resolve(ipv4_addr, out_mac)`: returns cached MAC or broadcasts ARP Request.
- **Acceptance Gate**: QEMU TAP/User networking test: host initiates ARP probe, FortressOS replies with MAC; FortressOS sends ARP query for gateway (`10.0.2.2`) and populates ARP table.

### Layer 4: IPv4 Protocol & Internet Checksum (RFC 791)
- **Scope**:
  - 20-byte IPv4 header parsing and construction: Version (4), IHL (>= 5), DSCP/ECN (0), Total Length, Identification, Flags/Fragment Offset (handle unfragmented; reject fragments initially), TTL, Protocol (`0x01` ICMP, `0x11` UDP, `0x06` TCP), Header Checksum, Source IP, Destination IP.
  - Standard RFC 1071 ones' complement Internet checksum calculation and verification.
  - Basic routing table: interface IP, subnet mask, default gateway IP.
    - If `dest_ip` matches local subnet: resolve target MAC directly via ARP.
    - If `dest_ip` is external: resolve gateway MAC via ARP.
- **Acceptance Gate**: Host unit tests (`tests/net_ipv4_host.c`): validates checksum algorithm, header parser, fragment rejection, and subnet routing logic.

### Layer 5: ICMP (Internet Control Message Protocol, RFC 792) & Ping
- **Scope**:
  - ICMP Header: Type (1B), Code (1B), Checksum (2B), Rest of Header (4B: Identifier, Sequence Number), Data payload.
  - Ingress:
    - Type 8 (Echo Request): automatically format and transmit Type 0 (Echo Reply) with identical Identifier, Sequence Number, and payload.
  - Egress:
    - Kernel API: `icmp_send_echo_request(dest_ip, id, seq, payload, len)`.
    - User tool `/bin/ping`: sends Echo Requests, waits for Echo Reply, calculates Round-Trip Time (RTT) using `apic_timer_get_bsp_ticks()` (`src/arch/x86_64/apic_timer.h`), and prints status to stdout.
- **Acceptance Gate**: **First major milestone win**:
  1. Host pings FortressOS in QEMU (`ping 10.0.2.15` succeeds with replies).
  2. Ring 3 command `run /bin/ping 10.0.2.2` executes from the FortressOS shell, reporting successful ping replies and accurate RTT.

### Layer 6: UDP Protocol & Socket Demux (RFC 768)
- **Scope**:
  - 8-byte UDP header: Source Port, Destination Port, Length, Checksum.
  - Validate length against IPv4 payload and verify UDP checksum (if non-zero).
  - Bounded kernel socket table (e.g. 16 UDP sockets): maps local port to socket structure containing a packet receive queue and wait channel.
  - Syscall integration: `SYS_SOCKET(AF_INET, SOCK_DGRAM)`, `SYS_BIND`, `SYS_SENDTO`, `SYS_RECVFROM`.
  - Non-blocking and blocking reads using `sched_wait_until()` (`src/kernel/thread.c:633`).
- **Acceptance Gate**: UDP Echo client/server test under QEMU; `/bin/udptest` sends UDP packet to host server and receives response.

---

## 4. Protected Contracts: DMA, IRQs, Locks, and Blocking

### 4.1 DMA Ownership, Quiescence, and Quarantine (Contract M4, §4 & §9)

The kernel storage subsystems (NVMe and xHCI) establish strict DMA contracts:
- `AGENTS.md:254` (M4): *"Never free DMA memory while a controller may still use it."*
- `PROTECTED.md:30-31`: *"`ENABLE_*` raw-write gates, disposable fixture separation, hardware storage exclusions, and DMA quarantine."*

The network driver must strictly follow this discipline:
1. **Allocation & Virtual Mapping**:
   - All descriptor rings and packet buffers must be allocated from PMM (`pmm_alloc_page()`, `src/mm/pmm.h:21`).
   - Virtual addresses must be derived via `vmm_phys_to_virt(phys)` (`src/mm/vmm.h:103`), never by casting raw physical integers (Contract M1, `AGENTS.md:251`).
2. **Two-Stage PMM/VMM Timing**:
   - **Zero NIC DMA buffers may be allocated during early `kmain`** while PMM is capped at 1 GiB (`PMM_BOOT_ALLOC_LIMIT`, `src/mm/pmm.h:9`).
   - Allocation must occur exclusively after `pmm_unlock_high_memory()` inside `vmm_init()` (`src/kernel/main.c:4260`), when the kernel PML4 covers all managed RAM up to 32 GiB.
   - Verify whether the controller requires 32-bit physical DMA addressing: Intel e1000/e1000e/I219-LM possesses 64-bit descriptor base registers (`TDBAH`/`RDBAH`). However, if a legacy hardware revision restricts DMA to 32 bits, ring and buffer allocations must be verified `< 4 GiB`.
3. **Quiescence Procedure**:
   - Before resetting the NIC, unregistering the device, or halting, the driver must:
     a. Disable RX and TX engines by clearing `RCTL.EN` and `TCTL.EN`.
     b. Clear interrupt mask registers (`IMC` = `0xFFFFFFFF`).
     c. Disable PCI Bus Mastering (`PCI_COMMAND_BUS_MASTER` in `PCI_REG_COMMAND`, `src/drivers/pci.h:34-37`).
     d. Perform a bounded polling loop (e.g. up to 10 ms via `delay_ms()`, `src/drivers/xhci.c:48-63`) ensuring all in-flight DMA operations complete.
4. **Quarantine on Failure**:
   - If the controller fails to halt or times out during quiescence:
     - The driver sets `g_net_fatal = true`.
     - Bus mastering remains permanently disabled.
     - **All physical DMA frames (rings and packet buffers) are permanently quarantined**: they are marked unfreeable and NEVER returned to PMM.
     - This mirrors xHCI (`src/drivers/xhci.c:616-620`: `"controller halt timed out; DMA frames quarantined"`) and NVMe (`src/drivers/nvme.c:13`: `g_dma_quarantined = true`).

### 4.2 IRQ vs. Polling Discipline & Ingress Routing (Contracts I1, I2, §4 & §9)

- `AGENTS.md:243` (I1): *"Ordinary device IRQ handlers do bounded draining/queue publication/wakeup only: no allocation, blocking, context switch or normal logging."*
- `AGENTS.md:244` (I2): *"Exactly one EOI owner. `idt_register_hardware_handler` makes the dispatcher own EOI. Spurious APIC IRQ gets no EOI."*

**Comparison with Storage Drivers**:
- *xHCI & NVMe*: Use polling under IRQ-save locks for block completions (`src/drivers/xhci_bot.c`, `AGENTS.md:101`). This works because disk I/O is CPU-initiated: the CPU requests an LBA read and synchronously waits for the response.
- *Network Ingress*: Unlike disks, network ingress is **asynchronous and unprompted**: external packets arrive at unpredictable times. Polling in a tight loop across the entire system lifetime would burn 100% of CPU cycles.

**Ingress Architecture**:
1. **Early Bring-up**: Strictly **polling mode** (`net_poll_rx()`). Direct calls in thread context with IRQs disabled. Proves descriptor mechanics without touching IDT, IOAPIC, or MSI.
2. **Interrupt-Driven Ingress**:
   - Vector registration: Use `idt_register_hardware_handler()` (`src/arch/x86_64/idt.h:68`).
   - EOI discipline: The IDT dispatcher automatically issues LAPIC EOI via `g_needs_eoi` (`src/arch/x86_64/idt.c:230`). The NIC ISR must **never** call `lapic_eoi()`.
   - SMP Routing: Pin network IRQ delivery strictly to the BSP (CPU 0), mirroring the single-core ingress discipline of keyboard/serial input and S8 pipe peers (`AGENTS.md:27-28`).
   - Handler Body (Strict I1 Compliance):
     ```c
     void net_irq_handler(interrupt_frame_t *frame) {
         (void)frame;
         /* 1. Read ICR to clear interrupt status in NIC */
         uint32_t icr = mmio_read32(E1000_REG_ICR);
         if ((icr & E1000_ICR_RXT0) == 0) return;

         /* 2. Bounded ring check: set pending flag or enqueue raw index */
         g_net_rx_pending = true;

         /* 3. Wake deferred ingress thread (no allocation, no logging, no locks) */
         sched_wake_all(&g_net_ingress_channel);
     }
     ```
   - Actual packet buffer parsing, protocol demux, ARP updates, and socket queueing take place in a dedicated kernel worker thread (`net_worker_thread`), running outside interrupt context.

### 4.3 Lock Hierarchy & Lock Ranks (Contract L1, `src/include/spinlock.h:34-59`)

The binding lock hierarchy in `spinlock.h:35-39` specifies:
```
Level 1: per-CPU scheduler locks OR ext2_lock OR g_process_lock
Level 2: g_heap_lock    (Kernel heap & free list)
Level 3: g_vmm_lock     (Page tables & virtual mapping)
Level 4: g_pmm_lock     (Physical frame bitmap allocator)
Level 5: g_console_lock (Leaf output lock)
```
*Rule*: Acquire in strictly increasing rank order; release LIFO; never hold a spinlock across `switch_context()`.

**Placement of Network Locks**:
The network stack requires synchronization for:
1. `g_net_dev_lock`: Protects NIC hardware TX/RX ring head/tail indices and descriptor updates.
2. `g_socket_table_lock`: Protects socket lookup, binding, and state transitions.

**Rank Analysis**:
- Sockets and network drivers frequently allocate dynamic memory (`kmalloc` / `kfree`, Rank 2).
- If network locks had Rank 2 or higher, acquiring `g_heap_lock` while holding a network lock would be an illegal rank inversion!
- Therefore, network locks must have **Rank 1**:
  - `g_net_dev_lock`: `SPINLOCK_RANKED(1, "net_dev")`
  - `g_socket_table_lock`: `SPINLOCK_RANKED(1, "socket_table")`
- **Strict Rank-1 Non-Nesting Invariant**:
  - As defined in `spinlock.h:40-42`, Rank-1 locks **never nest with each other**:
    - `g_net_dev_lock` CANNOT be acquired while holding `g_sched_lock`, `ext2_lock`, or `g_process_lock`.
    - `g_socket_table_lock` CANNOT be acquired while holding `g_net_dev_lock`.

### 4.4 Socket Blocking I/O (Contract S3b & S3c, §4)

- `AGENTS.md:235` (S3b): *"Sleep releases all locks before switching. Wake rechecks predicate."*
- `AGENTS.md:236` (S3c): *"Do not invent a `wait_queue_sleep` API — see `input_read` / `sched_wait_until`."*

When a socket operation blocks awaiting packet reception (`SYS_RECVFROM`):
- It must **not** invent an ad-hoc sleeping mechanism or custom queue sleeper.
- It must release `g_socket_table_lock` before sleeping.
- It must invoke `sched_wait_until(&sock->wait_channel, socket_has_data_predicate, sock)` (`src/kernel/thread.c:633`).
- On resume, the thread re-acquires `g_socket_table_lock` and re-evaluates queue emptiness.

---

## 5. Subsystem Initialization Ordering in `kmain`

The protected sequence in `src/kernel/main.c:4032-5676` dictates strict dependency ordering:
1. `serial_init()` (COM1, line 4035)
2. `console_init()` (Early framebuffer, line 4065)
3. `gdt_init()` & `idt_init()` (lines 4090, 4093)
4. `pmm_init()` (line 4171) — **Caps allocation at 1 GiB (`PMM_BOOT_ALLOC_LIMIT`, `src/mm/pmm.h:9`)**
5. `boot_info_init()` (line 4244) — Deep-copy Limine boot metadata
6. `vmm_init()` (line 4260) — **Builds master kernel PML4, switches CR3, unlocks PMM high memory (`pmm_unlock_high_memory()`, `src/mm/pmm.h:17`)**
7. `heap_verify_integrity()` (line 4877)
8. `acpi_init()` & `power_init()` (line 4942)
9. `ioapic_init()` & `apic_timer_init(100)` (line 5001)
10. `sched_init()` (line 5032)
11. `test_phase9a_pci_discovery()` (line 5553)
12. `test_smp_piece1_ap_discovery()` (line 5602)
13. `input_init()` (line 5630)
14. `xhci_boot_probe()` & `usb_mount_production_storage()` (lines 5636, 5638)
15. `process_spawn("shell", ...)` (line 5655)

### Exact Placement of Network Initialization
Networking **must not** initialize before Step 6 (`vmm_init`), because NIC MMIO mapping and DMA buffer allocations require the kernel PML4 and access to physical RAM above 1 GiB.
Networking also requires interrupts and scheduler services, so it must follow Step 9 (`ioapic_init`) and Step 10 (`sched_init`).

**Target Location**: In `src/kernel/main.c`, directly alongside the storage and USB probes (around lines 5635–5638):
```c
/* Post-SMP, post-input subsystem bring-up: */
boot_status("Probing network controllers...");
pci_report_nic();               /* Report discovered NICs in PCI inventory */
net_boot_probe(&boot_info);     /* Initialize e1000/I219 NIC, rings, and IP stack */
```

---

## 6. User-Facing Surface & Syscall ABI

Ring 3 user programs access networking through two mechanisms:

### 6.1 Syscall Numbers & ABI

Existing syscall numbers end at `SYS_SYSINFO = 37` (`src/include/syscall_abi.h:45`). New networking syscalls begin at 38:

| Number | Macro | Signature | Description |
| --- | --- | --- | --- |
| 38 | `SYS_SOCKET` | `(int domain, int type, int protocol) -> int fd` | Allocate socket descriptor |
| 39 | `SYS_BIND` | `(int fd, const struct sockaddr *addr, uint32_t addrlen) -> int` | Bind socket to local port |
| 40 | `SYS_SENDTO` | `(int fd, const void *buf, size_t len, int flags, const struct sockaddr *dest, uint32_t addrlen) -> int64_t` | Send packet to destination |
| 41 | `SYS_RECVFROM` | `(int fd, void *buf, size_t len, int flags, struct sockaddr *src, uint32_t *addrlen) -> int64_t` | Receive packet with sender info |
| 42 | `SYS_NETCTL` | `(int cmd, void *arg, size_t arg_size) -> int` | Interface info, IP/gateway config, ping stats |

### 6.2 Contract S1 & S2 Compliance
- **S1 (ABI registers)**: Arguments passed in RDI, RSI, RDX, R10, R8, R9. Results returned in RAX (`src/kernel/syscall.c:1-30`).
- **S2 (Range Validation)**: Every user-supplied pointer (`buf`, `addr`, `dest`, `src`) must be strictly validated before access using `vmm_validate_user_range(pml4, ptr, size, write_req)` (`src/mm/vmm.h:104`).
  - Kernel read buffers (`sendto` data, `bind` address): `write_req = false`.
  - Kernel write buffers (`recvfrom` buffer, `src` address): `write_req = true`.
  - Null pointers, unmapped ranges, kernel-space addresses (`>= 0xFFFF800000000000`), or arithmetic overflow return `-SYSCALL_EFAULT` immediately without dereference.

### 6.3 Standalone Ring 3 Utilities
Following the recipe in `AGENTS.md:428-438` (§7.6), networking tools are standalone static ELF binaries placed in `/bin`:
1. `/bin/ifconfig`: Displays interface name, MAC address, IPv4 address, subnet mask, gateway, and TX/RX packet statistics.
2. `/bin/ping <ip>`: Sends ICMP Echo Requests once per second, prints RTT in milliseconds, and summarizes packet loss on termination (Ctrl-C or count limit).
3. `/bin/udptest`: Minimal UDP client for echo transmission and verification.

---

## 7. Phasing & Milestone Breakdown

To ensure tight scope control and empirical de-risking, the roadmap is partitioned into two distinct milestones:
- **Milestone NET-1**: Foundation, Driver, ARP, IPv4, ICMP Ping, UDP Sockets, and Dell Acceptance.
- **Milestone NET-2**: Minimal Client TCP Streaming (deferred to a subsequent milestone).

### Milestone NET-1 Phasing

| Phase | Title | Primary Deliverable | Acceptance Gate |
| --- | --- | --- | --- |
| **Phase 0** | Core Abstractions & Buffers | `net_dev_t`, packet buffer pool (`pbuf_t`), checksum library | Host ASan/UBSan unit tests (`make test-net-host`) |
| **Phase 1** | PCI Discovery & MMIO (QEMU & Dell) | Detect Intel NIC, map BAR0 aperture, read MAC address | QEMU BIOS/UEFI & Dell Latitude 5590/5500 boot logs display NIC model and MAC |
| **Phase 2** | Rings & Raw Frame I/O | Allocate TX/RX rings, send raw frame, poll RX frame | QEMU `-netdev dump` pcap audit + Dell raw frame loopback / link-up confirmation |
| **Phase 3** | Ethernet & ARP | 14-byte Ethernet framing, ARP cache, ARP Request/Reply | QEMU gateway ARP resolution verified |
| **Phase 4** | IPv4 & ICMP (The Ping Win) | IPv4 parser/checksum, ICMP Echo Reply, `/bin/ping` | Host pings QEMU guest; `/bin/ping 10.0.2.2` succeeds; Dell cable ping to router |
| **Phase 5** | UDP & Socket Syscalls | UDP protocol, socket table, `SYS_SOCKET`/`SENDTO`/`RECVFROM` | Ring 3 `/bin/udptest` verified under QEMU and Dell hardware — **Closes Milestone NET-1** |

---

## 8. Push-Back Points & Reviewer Critique

This section directly examines the three core architectural push-back points raised for critique:

### 8.1 Critique Point 1: Polling vs. IRQ, MSI/MSI-X, and the Missing ACPI `_PRT`

**The Problem**:
The plan specifies BSP-pinned interrupt delivery in Phase 3+. However, FortressOS has **no AML interpreter** and does not parse ACPI `_PRT` (PCI Routing Tables) in DSDT (`src/drivers/acpi.c:300-380`).
On UEFI bare-metal machines like the Dell Latitude 5590 / 5500, firmware often leaves the legacy PCI configuration register `PCI_REG_INTERRUPT_LINE` unprogrammed, stale, or set to `0xFF` because modern UEFI firmware expects operating systems to use Message Signaled Interrupts (MSI or MSI-X). Attempting to route an unverified GSI via `ioapic_route_gsi()` could easily route to the wrong pin or receive no interrupts at all.

**Resolution & Strategy**:
1. **Tier 1 (Guaranteed Baseline — Polling Mode)**:
   - Polling mode (`net_poll_rx()`) requires zero IRQ routing and zero IOAPIC involvement.
   - Just as Phase 9G.2 mass storage achieved full physical read/write acceptance via bounded polling (`src/drivers/xhci_bot.c:445-450`, `AGENTS.md:101`), the network stack can achieve initial raw frame I/O, ARP, and ping completely in polling mode.
2. **Tier 2 (Interrupt Mode via MSI Capability)**:
   - Rather than guessing legacy IOAPIC GSI lines, the Intel e1000/e1000e/I219-LM controllers support standard PCI MSI (Capability ID `0x05`).
   - MSI bypasses the IOAPIC entirely: the driver writes the Local APIC destination address (`0xFEE00000 | (lapic_id << 12)`) and delivery vector directly into the NIC's PCI configuration registers.
   - This eliminates dependency on ACPI `_PRT` and provides deterministic BSP delivery without legacy pin sharing.

### 8.2 Critique Point 2: Staging Dell Hardware Acceptance (Early vs. Late)

**The Problem**:
In the original draft, Dell hardware acceptance was postponed until Phase 6 (after UDP and sockets).
The xHCI storage milestone demonstrated that hardware surprises (e.g. PHY clock gating, CSME power states, undocumented descriptor quirks) can invalidate assumptions made while developing solely in QEMU. If Dell hardware bring-up is delayed until after UDP is built, discovering a hardware-level PHY blocker requires expensive debugging and potential redesign.

**Resolution & Strategy**:
Interleave hardware acceptance checkpoints throughout NET-1 rather than deferring to the end:
- **Phase 1b Gate (Dell Discovery)**: Confirm PCI detection, BAR0 mapping, and MAC address readout on the physical Dell 5590/5500. Proves MMIO reads without risking DMA.
- **Phase 2b Gate (Dell Link & Raw Frame)**: Confirm PHY link-up (`STATUS.LU == 1`) and raw frame transmission on Dell hardware before writing ARP/IPv4.
- **Phase 4b Gate (Dell Ping Acceptance)**: Run `/bin/ping` on Dell hardware over a physical Ethernet cable to a real home/office router. This proves the entire driver and Layer 2–4 stack before tackling UDP sockets.

### 8.3 Critique Point 3: Deferring TCP to a Dedicated Milestone (NET-2)

**The Problem**:
TCP includes connection state machines, dynamic timeout calculations, retransmissions, duplicate ACKs, window probing, and flow control. In many systems, attempting to squeeze TCP into an initial networking milestone leads to an unfinished or fragile implementation that stalls the entire project.

**Resolution & Strategy**:
- **Milestone NET-1 Boundary**: Strictly ends at Phase 5 (UDP + Ping + Socket ABI + Dell Hardware Acceptance). Reaching a working `/bin/ping` and `/bin/udptest` on both QEMU and bare-metal Dell hardware constitutes a complete, verifiable, and robust proof-of-concept.
- **Milestone NET-2 (TCP Arc)**: TCP is formally designated as its own subsequent milestone, building upon the rock-solid `net_dev_t` and IPv4 foundations of NET-1.

---

## 9. Risks, Unknowns, and Hardware Facts

1. **Dell Intel I219-LM PHY Negotiation & MDIC**:
   - The I219-LM connects the MAC in the Intel PCH to an external PHY over PCIe/CSME.
   - On Kaby Lake-R / Cannon Lake platforms, firmware or CSME often leaves the PHY in a low-power clock-gated state.
   - Resetting the MAC without properly negotiating the PHY via the MDIC (MDI Control) register can cause link detection to fail (`STATUS.LU = 0`).
   - *Mitigation*: Read status registers and link state early. If link is down, execute the documented Intel MDIC PHY reset sequence.
2. **Two-Stage PMM/VMM Allocation Boundary**:
   - PMM allocations are capped at 1 GiB (`PMM_BOOT_ALLOC_LIMIT`) until `pmm_unlock_high_memory()` inside `vmm_init()`.
   - *Mitigation*: Enforce static and runtime checks ensuring no network DMA rings or packet buffers are allocated during early `kmain`. All network initialization is placed strictly post-VMM and post-SMP.
3. **Descriptor Ring Memory Bounds**:
   - Ring descriptors and packet buffers must not span page boundaries unpredictably.
   - *Mitigation*: Each ring fits comfortably in a single 4 KiB frame (64 entries × 16 bytes = 1024 bytes). Packet buffers are allocated as individual contiguous frames.
4. **SMP Safety & Multi-CPU Contention**:
   - In SMP runs (`SMP=4`), packet ingress interrupts could theoretically arrive on APs.
   - *Mitigation*: For initial phases, route the NIC interrupt strictly to the BSP (CPU 0), mirroring the input/pipe BSP-affinity model (`AGENTS.md:27`).

---

## 10. Explicit Non-Goals ("What Networking Does NOT Do")

To prevent scope creep and maintain architectural boundaries, the following features are explicitly **out of scope for Milestone NET-1**:

- **No IPv6**: IPv4 only. IPv6 address autoconfiguration and neighbor discovery are deferred.
- **No Wireless / Wi-Fi (802.11)**: Wired Ethernet (802.3) only.
- **No TCP in Milestone NET-1**: TCP is deferred to Milestone NET-2.
- **No In-Kernel TLS / HTTPS**: No cryptographic handshakes in Ring 0.
- **No Dynamic DHCP Client in NET-1**: Fixed static IP configuration (e.g. `10.0.2.15/24`, gateway `10.0.2.2` for QEMU) initially; DHCP client deferred to a later userland utility.
- **No Hardware Offloads**: No TCP Segmentation Offload (TSO), Large Receive Offload (LRO), or hardware checksum offload. All checksums calculated in software.
- **No Multiple Active NICs or Bridging/Bonding**: Single primary network interface (`eth0`).
- **No Hot-Plug NIC Enumeration**: Controller must be present at boot.
