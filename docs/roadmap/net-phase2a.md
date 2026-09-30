# NET Phase 2a — descriptor DMA and raw Ethernet I/O

Complete 2026-09-30, QEMU only. Phase 2b (Dell raw frame/DMA acceptance) is next.
No network interrupts, worker, protocol processing or socket syscalls are added.

## Reading pass and approved API correction

The existing xHCI driver allocates PMM frames, uses `vmm_phys_to_virt()` for
CPU pointers and physical addresses for controller ring bases. Its failed halt
path disables PCI mastering and retains allocations. `pmm_alloc_page()` returns
a physical address (zero on OOM); the existing VMM/HHDM covers managed RAM after
high-memory unlock. No PMM/VMM changes were needed.

Phase 1a's device retains PCI identity, BAR physical address, mapped MMIO base,
aperture, MAC, STATUS, link state and initialization state. These are extended
by private driver ring/pool state rather than replaced.

The Intel 8254x Software Developer's Manual chapters 3 and 14 define 16-byte
legacy TX/RX descriptors, 16-byte ring-base alignment and 128-byte ring-length
granularity. The 1024-byte rings satisfy these constraints. Compile-time size
assertions check the actual C layouts.
Reference: https://www.intel.com/content/dam/doc/manual/pci-pci-x-family-gbe-controllers-software-dev-manual.pdf

The reading pass found `net_dev_t.poll_rx` returned `int`, so it could not return
the requested detached packet. Implementation paused and the user explicitly
approved changing it to `pbuf_t *` and adding an explicit recycling callback.
`recycle_rx(dev, packet)` returns a caller-owned packet to the driver spare pool.
No other users of the old callback existed.

## Allocation, initialization and ownership

- TX/RX: 64 legacy descriptors each, one 4 KiB PMM page per ring.
- RX: 80 pages, each containing one `pbuf_t`; initially 64 are attached and 16
  are spares. Descriptor addresses are retained page physical addresses plus
  `offsetof(pbuf_t, data)`. The full 2048-byte buffer fits within its page.
- TX: 64 permanent driver-owned pages. Each raw send copies the caller's bytes
  into its descriptor's page; caller memory is never exposed to DMA.
- Total: 146 pages / 584 KiB retained until reboot. Failed partial allocation
  also retains its allocated pages. No DMA page is returned to PMM.

Initialization remains at the existing post-SMP network probe, after VMM and
PMM high-memory unlock. Only PCI IDs 8086:100E and 8086:10D3 enable this DMA path;
I219 devices remain discovery-only pending Phase 2b.

IMC masks interrupts; PCI INT_DISABLE remains set. Disable RX/TX, populate and
zero DMA storage, program TDBAL/TDBAH/TDLEN and RDBAL/RDBAH/RDLEN, initialize
heads/tails (TX 0/0, RX 0/63), clear RFCTL extended-descriptor selection, set
TIPG 10/8/6, TCTL EN/PSP/CT=15/COLD=64, and RCTL EN/BAM/2048-byte buffers/SECRC.
No promiscuous bits are set. Flush posted MMIO writes and enable/read back
PCI bus mastering only after all descriptors and engine registers are ready.
Publish one `eth0` device with MAC, MTU 1500, flags and raw TX/RX/recycle callbacks.

`g_net_dev_lock` is ordinary rank 1. Callbacks require unlocked thread context;
there is no scheduler operation or logging under this lock. Initialization
allocates outside the device lock. Runtime descriptor and pool changes are
serialized. CPU DMA memory is coherent on x86; volatile status loads observe
writeback, and compiler barriers order descriptor/payload access around DD and
MMIO doorbells.

TX validates 14..1514 bytes and current link state, reserves the next descriptor,
copies into driver-owned memory, sets EOP/IFCS/RS, clears DD and advances TDT.
The waiter retains a software reservation until it consumes completion, so a
ring wrap cannot overwrite a descriptor while its original waiter is pending. At most 63 descriptors may be pending; one entry stays unused so head/tail equality never disguises a full TX ring as empty.
Completion polling drops the lock between checks and has a 1,000,000-iteration
cap (an iteration bound, not a calibrated millisecond guarantee).

RX checks DD, EOP, errors and frame bounds. It detaches a valid packet only when
it can replace the descriptor with a spare. Replenish then clear descriptor
status and advance RDT. Invalid frames and spare exhaustion drop/rearm the
original buffer. Multi-descriptor frames are discarded through their EOP;
a final fragment is never exposed as a complete frame. Recycling checks pool
identity and detached state, rejecting duplicate/foreign/ring-owned returns.

## Fatal stop and quarantine

TX timeout or bring-up failure latches `g_net_fatal`, clears public flags,
disables RX/TX, masks interrupts and disables PCI bus mastering. There is no
xHCI-style HALTED register on this NIC: a bounded CTRL.RST completion poll is
used instead of claiming engine-disable readback proves DMA quiescence.
Mastering is disabled again and read back after reset. Reset timeout returns
failure; all DMA allocations remain permanently retained even if reset succeeds.
The driver never recovers/re-registers after a fatal error, and cannot recycle
packets into a fatal controller. Hardware reset/quarantine failure is covered
by host register mocks, not by a physical NIC failure experiment.

## QEMU gate and complete output

Command: `wsl -d Ubuntu-24.04 -- make test-net-rings`, SMP=1.
The runner builds a disposable ISO enabling only the `net_test=rings` hook,
uses disposable paired OVMF vars/code, and permits no data disks. Exact final
argv comparison rejects added disks, devices, network backends and dump objects;
negative injection checks run for every case.

A loopback UDP socket backend carries raw Ethernet frames, permitting both TX
capture and host injection. `filter-dump` creates `build/test-net-rings-*.pcap`.
The host checks the full known 60-byte TX packet, including addresses, EtherType,
payload and padding, against the pcap. The host injects the corresponding RX
packet and the actual kernel RX callback checks all 60 bytes and recycles it.
The pcap must also contain the exact injected RX packet. This substitutes the
socket backend for the proposed user backend so real RX can be tested; there is
no guest UDP/IP protocol logic.

```text
[PASS] bios-e1000 SMP=1: TX DD + pcap 60/60 byte match; RX injected 60/60 byte match + recycle; shell ready
  [NET 2a] eth0: TX/RX 64x16, RX pool 80, DMA ready (polling only)
  [NET 2a] TX PASS: 60-byte raw frame, DD observed
  [NET 2a] RX waiting
  [NET 2a] RX PASS: 60 bytes byte-checked; buffer recycled
[PASS] bios-e1000e SMP=1: TX DD + pcap 60/60 byte match; RX injected 60/60 byte match + recycle; shell ready
  [NET 2a] eth0: TX/RX 64x16, RX pool 80, DMA ready (polling only)
  [NET 2a] TX PASS: 60-byte raw frame, DD observed
  [NET 2a] RX waiting
  [NET 2a] RX PASS: 60 bytes byte-checked; buffer recycled
[PASS] uefi-e1000 SMP=1: TX DD + pcap 60/60 byte match; RX injected 60/60 byte match + recycle; shell ready
  [NET 2a] eth0: TX/RX 64x16, RX pool 80, DMA ready (polling only)
  [NET 2a] TX PASS: 60-byte raw frame, DD observed
  [NET 2a] RX waiting
  [NET 2a] RX PASS: 60 bytes byte-checked; buffer recycled
[PASS] uefi-e1000e SMP=1: TX DD + pcap 60/60 byte match; RX injected 60/60 byte match + recycle; shell ready
  [NET 2a] eth0: TX/RX 64x16, RX pool 80, DMA ready (polling only)
  [NET 2a] TX PASS: 60-byte raw frame, DD observed
  [NET 2a] RX waiting
  [NET 2a] RX PASS: 60 bytes byte-checked; buffer recycled
All 4 NET Phase 2a QEMU cases PASS. RX coverage: one injected raw frame per case; no worker/protocol/hardware claim.
```

## Host coverage and regressions

`make test-net-rings-host` (also a dependency of `test-net-rings`) compiles the
actual driver with mocked PMM/HHDM, PCI, MMIO and pthread locks under ASan/UBSan.
It checks the high-memory guard, all 146 allocation-failure boundaries,
ring geometry/physical addresses, no repeated bring-up, RX spare exhaustion,
detachment/replacement/recycling identity, 130-frame RX wrap, invalid lengths,
errors and fragment rejection; 65 TX submissions exercise wrap, driver-owned
copies and waiter reservations. Missing completion and stuck reset test fatal
state, mastering disable and retained allocations. Mock completion is not a
hardware/SMP claim.

Regression results on 2026-09-30, Ubuntu-24.04 WSL:

| Command | Result / scope |
| --- | --- |
| `make test-net-host` | PASS, 103/103 ASan/UBSan codec/buffer cases |
| `make test-net-pci` | PASS, 4/4 BIOS/UEFI present/absent, SMP=1, no data disks |
| `make test-net-rings-host` | PASS, actual driver with mocked hardware, ASan/UBSan |
| `make test-net-rings` | PASS, 4/4 BIOS/UEFI × e1000/e1000e, SMP=1, no data disks |
| `make test-nmi` | PASS BIOS/UEFI, 28 syscall-boundary and 24 sigreturn-boundary exact NMIs per firmware; recovery checks; existing NVMe fixture scope |
| `make test-shell` | PASS BIOS/UEFI input/lifecycle and S3/S4 suites plus keyboard-only UEFI; existing NVMe fixture scope. Logs confirm default q35 e1000e 8086:10D3 and initialized rings |
| `make` | PASS strict freestanding build and raw-image integrity validation |

Shell regression note: one rerun failed at `scripts/test_shell.py:197` in the held-stop snapshot (`stdin reader must sleep, not yield/poll`). The preceding full suite had passed. The runner resumes the guest after finding a blocked shell, then stops it again for the snapshot; a timed-input wake can occur in that gap. This is a plausible sampling race, not proven as the sole cause. No scheduler/input code or assertion was changed; the unchanged full rerun then passed BIOS/UEFI input/lifecycle/resource checks, keyboard-only UEFI, and all S3/S4 checks.

Artifacts: `build/test-net-rings-{bios,uefi}-{e1000,e1000e}.{log,pcap,stderr}`.
The self-test sends an experimental EtherType 0x88B5 frame only with the exact
boot token. Normal boots do not send a test frame.

## Files changed

- `src/drivers/e1000.c` / `.h`: rings, PMM DMA storage, raw TX/RX, recycling, bounded fatal stop, explicit boot self-test.
- `src/include/net.h`: approved packet-returning RX callback and recycle callback.
- `src/kernel/main.c`: invoke the opt-in self-test beside the existing network probe.
- `scripts/test_net_rings.py`: dedicated raw-frame runner, reusing Phase 1a repository/OVMF constants; socket injection and pcap audit.
- `tests/net_rings_host.c` / `Makefile`: actual-driver sanitizer coverage and QEMU make targets.
- `AGENTS.md`, this file and `docs/roadmap/README.md`: phase status, target scope and evidence.

## Boundaries and next work

RX acceptance covers one injected frame per QEMU case. Burst/wrap/failure
ownership coverage uses host mocks; no sustained QEMU traffic, worker cadence,
protocol delivery, real cross-core execution or physical NIC DMA is claimed.
No changes to PMM/VMM, IRQ routing or scheduler contracts were required.

Phase 2b must establish physical DMA/link/raw-frame behavior and any I219-specific
initialization/quiescence requirements. The earlier Dell link observation is
preserved, but is not a Phase 2b frame-transmission pass. Phase 3 will drive the
polling callbacks from the BSP worker and connect the codecs.
