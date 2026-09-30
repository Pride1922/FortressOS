# NET Phase 2b — I219 PCH DMA bring-up

Status (2026-09-30): implementation delivered; **physical Dell acceptance pending**.
This document records implementation and mocked tests, not a physical TX or wire pass.
Acceptance machine: Dell Latitude 5590, observed I219-LM `8086:15D7`.

## Reading pass and sources

The checked-in plan had no §9.1. Its PHY/CSME risk rationale was in §7.2;
§9.1 has now been added with the implementation boundary and evidence sources.
Phase 1b proves discovery/link negotiation on one unit, not DMA/reset behavior.

The old `net_boot_probe()` guard only called `e1000_init_rings()` for `100E` and
`10D3`. These models still take the original 2a initialization and quiescence
branches. Only `15D7` (SPT/KBL LM4) and `15BD`/`15BB` (CNP LM6/LM7) enter the new
PCH takeover. Older `15B7`/`156F` IDs stay discovery-only.

The 32-bit MMIO BAR does not constrain DMA addresses. PMM physical ring bases
are split into `RDBAL/H` and `TDBAL/H`; descriptors contain physical 64-bit
addresses. CPU access still uses `vmm_phys_to_virt()`.

Primary references, pinned to Linux v6.12:

- [Intel I219 datasheet](https://cdrdv2-public.intel.com/612523/ethernet-connection-i219-datasheet.pdf)
  describes the PHY/PCH relationship and firmware-managed initialization. It
  does not replace the integrated MAC's generation-specific programming rules.
- [hw.h](https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/intel/e1000e/hw.h)
  and [device table in netdev.c](https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/intel/e1000e/netdev.c)
  establish the SPT/CNP device mapping.
- [ich8lan.c](https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/intel/e1000e/ich8lan.c):
  `e1000_acquire_swflag_ich8lan`, `e1000_reset_hw_ich8lan`,
  `e1000_initialize_hw_bits_ich8lan`, `e1000_init_hw_ich8lan`.
- [mac.c](https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/intel/e1000e/mac.c):
  `e1000e_disable_pcie_master`, `e1000e_config_collision_dist_generic`.
- [netdev.c](https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/intel/e1000e/netdev.c):
  `e1000_configure_tx`, `e1000_flush_desc_rings`, `e1000e_get_hw_control`.
- Register constants are checked against
  [regs.h](https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/intel/e1000e/regs.h),
  [defines.h](https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/intel/e1000e/defines.h)
  and [ich8lan.h](https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/intel/e1000e/ich8lan.h).

## Ported operations and scope decisions

| Operation | Implemented | Rationale / boundary |
| --- | --- | --- |
| Generation selection | SPT `15D7`, CNP `15BD`/`15BB` | No generic all-I219 gate |
| Link prerequisite | Bounded 500 ms `STATUS.LU` poll before/after reset | Preserve the negotiated PHY; stop when absent |
| Pending transactions | `CTRL.GIO_MASTER_DISABLE`, bounded 100 ms `STATUS.GIO_MASTER_ENABLE` wait | Do not reset over pending master requests |
| Engine disable | IMC all masked, RCTL off, TCTL PSP, posted-write flush, 10 ms delay | Reference pre-reset ordering |
| Descriptor flush hazard | Set `FEXTNVM11.DISABLE_MULR_FIX`; read PCI `0xE4` bit 8 with TDLEN | If flush is required, skip reset and quarantine; no dummy DMA submission |
| Ownership | Wait SWFLAG clear up to 100 ms, request and verify up to 100 ms | Never steal an existing semaphore; shorter fail-fast cap than Linux's 1000 ms acquisition cap |
| Reset | MAC-only CTRL.RST, no MMIO access for 20 ms, bounded 100 ms completion poll | No PHY_RST; no immediate post-reset flush (reference warns it can hang) |
| CTRL_EXT | Required bit 22, RO_DIS bit 17, DRV_LOAD bit 28 | Preserve SMBus/PHY firmware state; announce host driver ownership |
| Coherent DMA | Clear GCR no-snoop bits 0–5 | Required for the existing x86 coherent DMA discipline |
| TXDCTL queues 0/1 | COUNT_DESC, GRAN, writeback threshold 1, prefetch 31 | Reference descriptor writeback configuration |
| TARC queues 0/1 | Required arbitration fields, MULR clear | Single queue; SPT additionally limits outstanding requests to two |
| SPT-only IOSFPC | Bit 16 | SPT/KBL corruption workaround; not applied to CNP |
| TCTL / TIPG | CT 15, COLD 63, PSP/RTLC/EN; IPGT 8 at 1 Gb/s, 12 at 10/100 | PCH transmit settings, separate from 2a values |
| RCTL / RFCTL | Broadcast+own-MAC, 2048-byte buffers, strip CRC; legacy descriptors and NFS filters disabled | No promiscuous, VLAN, jumbo or protocol offloads |
| ECC | PBECCSTS.ECC_ENABLE and CTRL.MEHE | Reference applies to LPT and later, including SPT/CNP |
| KABGTXD | BGSQLBIAS `0x50000` | Reference post-reset setup |
| Receive address | Restore captured RAL0/RAH0, clear 32-entry multicast hash | Preserve ME shared receive-address slots |
| Publication | Ring-base/length, engine, ECC, descriptor mode and link readbacks | Publish eth0 only after validation |

This is deliberately **warm-PHY MAC takeover**, not a full-chip PHY reset or
complete cold-start Linux driver port. `CTRL.PHY_RST`, MDIC writes, forced
SMBus changes, ULP exit, LANPHYPC power cycling, EEE/K1/LPLU/s0ix, WOL, NVM
recovery and later-generation DMA-clock workarounds are not ported. The
reference couples PHY reset to post-PHY configuration; doing only half that
sequence would be unsafe. Link failure stops instead of guessing recovery.
An invalid firmware MAC is rejected rather than using legacy EERD on PCH.

Delays use the existing xHCI-style PIT channel-2 pattern, locally in the driver,
with speaker/gate restoration and an iteration cap for broken timer hardware.
They require neither scheduler ticks nor IRQs. No PMM/VMM/scheduler/lock change.
Long PCH lifecycle waits are boot/fatal operations, never an ordinary IRQ path.

## Fatal stop and diagnostic record

The rank-1 device lock excludes concurrent TX/RX when latching fatal state.
The first failure record is captured before containment and never overwritten.
It stores STATUS, CTRL, CTRL_EXT, MDIC, EXTCNF_CTRL, FWSM, RCTL/TCTL, both
ring bases/lengths/heads/tails, TXDCTL/TARC0, RFCTL, ECC and FEXTNVM11, PCI
command/descriptor status and software RX/TX cursors/pending count. Register
offsets printed in the report correspond to `s_pch_diag_regs` in `e1000.c`.
Hardware heads/tails and software cursors are distinct; no producer index is
invented. No DMA pointers are dereferenced during reporting.

On TX or ring bring-up failure, mask interrupts and disable engines, then make
one bounded PCH reset attempt where safe. On an already failed link/reset/
ownership attempt, do not retry. Always clear PCI bus mastering and verify its
readback; a failure to clear is explicitly reported. All allocated DMA pages
remain retained even when reset succeeds. No further init or TX/RX after fatal.

If the 20 ms post-reset timer fails, no further MMIO is attempted: publish the
saved pre-reset register record, explicitly labelled PRE-RESET, and contain
through PCI config only. If descriptors require flushing, skip reset and retain
DMA rather than risking the reference's documented unit-hang condition.

After dropping the device lock, unmute the framebuffer and print the copied
record through `serial_puts()`. That existing API sends COM1, mirrors once to
the initialized framebuffer, and retains dmesg. No extra console acquisition
under the device lock; no changes to the serial/console subsystem. Reset and
bus-master containment results are printed separately from the immutable
failure-time register values. Shell startup proceeds after a stopped probe.

## Cable-side acceptance gate

Normal boot sends no frames. For the manual Dell test, edit the Limine kernel
command line before boot and append `verbose net_test=rings`. Prefer the existing
read-only recovery entry; retain its selected USB PARTUUID and `usb_data_mode=ro`.
This change does not alter the image's existing storage authorization/menu policy.

On a wired second machine on the same broadcast domain, start capture **before**
booting the Dell (replace `INTERFACE` with the cable-side interface):

```sh
sudo tcpdump -i INTERFACE -s 0 -U -w dell-net-2b.pcap 'ether proto 0x88b5'
```

The self-test sends exactly one 60-byte frame excluding FCS:

- Destination `FF:FF:FF:FF:FF:FF`; source is the discovered Dell MAC.
- EtherType `88 B5`.
- Bytes 14–31: ASCII `FORTRESS-NET-2B-TX` (18 bytes, no trailing NUL).
- Bytes 32–59: `A5` repeated 28 times.

In Wireshark or `tcpdump -nn -e -XX -r dell-net-2b.pcap`, check all 60 bytes
against that definition, including the observed source MAC. Some adapters
preserve an additional FCS; identify it explicitly rather than treating it as
payload. Kernel DD cannot establish wire observation and no loopback is assumed.

Acceptance requires Dell DD/link logs **and** the matching cable-side frame.
If only DD is observed, record: **TX completed, wire not observed**. If the
stop-condition fires, photograph/save its record, report the bounded outcome
and leave NET unavailable; do not reboot repeatedly to guess a workaround.

Physical evidence: the user reports a Dell 5590 attempt reaching polling DMA
ready, then TX timeout with TDH=0, TDT=1 and descriptor status=0. Reported
TXDCTL=0x0141001F, TCTL=0x0103F0FA and STATUS=0x00080483; the user reports
successful fatal containment. This is a failed acceptance attempt, not TX PASS
or an agent-observed hardware result. No cable-side capture has been supplied.
Phase 2b remains incomplete.

### TX threshold correction after the Dell attempt

The prior threshold mask omitted HTHRESH, preserving reset zero. Program
PTHRESH=31, HTHRESH=1, WTHRESH=1, GRAN=1 and the existing raw bit 22 setting
on both TX queues, giving 0x0141011F when other fields are zero. Check these
fields on initialization readback. The family-specific policy is documented
in [FreeBSD e1000 commit 59709be69b07](https://github.com/freebsd/freebsd-src/commit/59709be69b07).
Do not import the bit-25 queue-enable definition from igb/ixgbe into this
legacy PCH register. The public I219 PHY datasheet does not document TXDCTL.
TCTL already decodes to CT=15 and COLD=63; it needs no change.

For descriptor zero, copy all 16 volatile bytes before writing TDT and print
the copy after unlocking, alongside the expected physical buffer address.
For the 60-byte self-test, offsets 8/9 must be 3C/00, offset 11 must be 0B,
and offset 12 must be 00. Bytes 0–7 are the little-endian physical address.
The containment implementation is unchanged. This correction is supported
by the reference policy; whether it resolves this Dell stall requires a new
physical boot and wire capture.

Correction verification (2026-09-30): `make test-net-i219-host`,
`make test-net-rings-host` and `make test-net-host` PASS under sanitizers;
`make test-net-rings` PASS all four BIOS/UEFI × e1000/e1000e cases.
PCH mocks verify rejection when either TXDCTL write is ignored and retain
the pre-doorbell status-zero byte dump even with immediate mocked DD.
`make` rebuilt the kernel, ISO and raw image; GPT/ESP/ext2 image checks PASS.
No physical USB reflash or new Dell acceptance is claimed.

### Dell retry with HTHRESH=1 (user-supplied boot log)

The subsequent Dell 5590 log shows TXDCTL=0x0141011F, TDH=0, TDT=1,
STATUS=0x00080483 and TCTL=0x0103F0FA. Descriptor zero before TDT is
`00 D0 24 04 00 00 00 00 3C 00 00 0B 00 00 00 00`: physical buffer
0x0424D000 (matches the driver-owned page), length 60, EOP/IFCS/RS and DD clear.
TX still times out. The log reports MAC reset complete and PCI mastering
verified off, with DMA retained. The threshold correction is therefore
insufficient to resolve this hardware failure. There is no TX PASS or wire
acceptance. The repeated STOP is the same immutable record printed by the
send timeout and the self-test's second quiesce call; it is not evidence of
a second reset attempt.

The next build adds independent read-only TX checkpoints before TDT and on
timeout before containment, printing copies only with the device lock released.
Containment itself is unchanged. Fields: PBA (0x1000), PBS (0x1008), GCR
(0x5B00), IOSFPC (0xF28), queue-1 TXDCTL/TARC (0x3928/0x3940), TIDV/TADV
(0x3820/0x382C), FEXTNVM6/7/9 (0x10/0xE4/0x5BB4), and TDFH/TDFT/TDFHS/
TDFTS/TDFPC (0x3410/0x3418/0x3420/0x3428/0x3430). These expose packet-buffer
allocation, FIFO progress and clock/request setup omitted from the existing
failure record. No new activation bit, PHY reset, global IOMMU change or
unverified packet-buffer write has been added.

Reference: [Intel-maintained Linux e1000e regs.h](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/net/ethernet/intel/e1000e/regs.h)
defines the register offsets; [netdev.c](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/net/ethernet/intel/e1000e/netdev.c)
includes the TX FIFO register dump. In that driver, e1000e_reset explicitly
programs packet-buffer allocation, with SPT/CNP defaults supplied by ich8lan.c;
this driver currently inherits it. Measuring PBA is required before treating
that difference as the cause of this Dell stall. Other DMA blockers, including
firmware remapping, have not been excluded by this log.

### Dell FIFO checkpoint and post-reset workaround restoration

The user-supplied checkpoint log shows PBA=0x000E0012 (18 KiB RX,
14 KiB TX) and PBS=0x20. TDFH/TDFT initially both 0x900; at timeout TDFT
advances to 0x902 while TDFH remains 0x900, TDFPC remains zero, and TDH
remains zero. This establishes FIFO movement, not completed transmission
or the precise amount of payload fetched. A blanket "no DMA fetch" diagnosis
is no longer justified. Packet-buffer starvation is not indicated by the
14 KiB allocation. The descriptor and TXDCTL values remain correct.

FEXTNVM11 still reads 0xFB21C1C2, with bit 13 clear. The initial reset lost
the setting previously written only before reset. The next correction
reapplies the existing disable-MULR-fix/reset-hang workaround after reset,
before enabling the transmit engine, and requires readback before publishing
eth0. This mirrors [iPXE intel_open](https://raw.githubusercontent.com/ipxe/ipxe/master/src/drivers/net/intel.c),
which sets FEXTNVM11 for INTEL_RST_HANG before creating transmit/receive
rings; PCI 15D7 is tagged INTEL_I219 there. The independent pre-reset
workaround and fatal containment are unchanged. With other fields unchanged,
FEXTNVM11 should now be 0xFB21E1C2; it is included in both TX checkpoints.

This is a reference-backed startup correction, not proof of the cause of the
observed TX stall. No PHY register writes, power cycle or packet-buffer
reallocation has been introduced. Tests model bit 13 being cleared by reset,
require its restoration before TCTL.EN, and reject ignored workaround writes.
Dell TX DD and wire acceptance remain pending.

Post-reset correction verification: PCH and legacy rings host sanitizer
suites PASS; four QEMU rings cases PASS; final kernel/raw image rebuild and
GPT/ESP/ext2 verification PASS. Physical behavior remains unverified.

## Verification

`make test-net-i219-host`: PASS under ASan/UBSan, actual driver with deterministic
MMIO/PCI/PIT/PMM/pthread mocks. Covers all three generation IDs, required settings,
SPT-only errata, one-shot init, normal-boot silence and exact self-test payload,
TX timeout/reset, link/pending/ownership/reset/timer failure, allocation and
configuration failure (all 146 allocation boundaries), unsafe descriptor-flush stop, immutable records,
no MMIO inside reset blackout, no reports under the device lock, retained DMA,
mastering-off (including failed disable readback) and QEMU bypass. This proves
software decisions, not silicon timing.

Executed from PowerShell through `wsl -d Ubuntu-24.04 -- make ...`, in
`/mnt/c/Sources/FortressOS`, 2026-09-30. Final regression invocation was
`make -k test-net-host test-net-pci test-net-rings test-net-rings-host test-nmi test-shell`
and exited 0. The original sequential invocation stopped at the NMI failure
described below; the final rerun used the final kernel implementation.

| Command | Result / evidence boundary |
| --- | --- |
| `make test-net-i219-host` | PASS ASan/UBSan; deterministic mocks, no physical claim |
| `make test-net-host` | PASS 103/103 ASan/UBSan |
| `make test-net-pci` | PASS 4/4 BIOS/UEFI present/absent, SMP=1 |
| `make test-net-rings` | PASS 4/4 BIOS/UEFI × e1000/e1000e, SMP=1; TX DD + exact 60/60 pcap bytes, RX exact 60/60 injected bytes + recycle; shell ready |
| `make test-net-rings-host` | PASS ASan/UBSan actual 2a ownership/failure paths; also run as rings dependency |
| `make test-nmi` | Final rerun PASS BIOS + UEFI: 28 syscall-boundary NMIs and 24 sigreturn-boundary NMIs per firmware, plus recovery checks |
| `make test-shell` | PASS BIOS/UEFI input, blocked-reader/timer/lifecycle/resource checks; keyboard-only UEFI 8 GiB without UART; S3/S4 integration and history both firmware modes |
| `make` | PASS kernel/ISO/raw image; protective MBR, primary/backup GPT CRC, FAT32 ESP and ext2 `e2fsck` checks pass |
| `git diff --check` | PASS |

Initial NMI attempt: BIOS round 3, `syscall_exit_user_rsp`, failed the existing
`IRET did not restore interrupted state` assertion after earlier probes passed.
The cause is not established. A final-build rerun completed all BIOS/UEFI NMI
and sigreturn checks. No NMI/scheduler code or test assertion was changed; the
first failed attempt is not erased or promoted to a pass.

Artifacts: `build/test-net-pci-*.log`, `build/test-net-rings-*.log` and `.pcap`,
`build/nmi-{bios,uefi}.json`, `build/nmi-sigreturn-{bios,uefi}.json`,
`build/shell-{bios,uefi}-1cpu.log`, `build/shell-s3-s4-{bios,uefi}.log` and
`build/shell-keyboard-only.png`. The final raw image is `bin/fortress.img`;
building it wrote only workspace image files, not a physical device.

### Explicit store fencing and physical-derived descriptor readback

The next user-reported Dell run confirms FEXTNVM11=0xFB21E1C2, but TX
still times out with the same FIFO movement and no DD. Restoring bit 13
was therefore insufficient; no successful hardware transmission is claimed.

At the user's request, send_raw now executes `sfence` with a compiler
`memory` clobber immediately before the TDT write. A diagnostic store fence
also precedes copying descriptor zero through `vmm_phys_to_virt(s_tx_phys)`.
The allocated address is used, never the historical literal 0x041FB000.
Both byte copies are taken before the doorbell and printed after unlocking;
the output includes the physical ring address and both CPU virtual addresses.
TDBAL/TDBAH/TDLEN/TDH/TDT are now included alongside the FIFO checkpoints.
Containment, descriptor fields, TXDCTL and ring-programming order are unchanged.

Important interpretation limits: vmm_phys_to_virt returns the HHDM address,
and s_tx was established with that same function. This is the same CPU
mapping, not an uncached physical-memory probe or independent observation of
NIC-visible bytes. VMM maps RAM with PRESENT/WRITABLE/NX, without PCD/PWT;
the driver creates no extra descriptor alias. Normal cached RAM is intentional
for coherent DMA. SFENCE orders stores; it is not a full load/store barrier
and does not flush or invalidate cache lines. Equal dumps cannot establish
DMA coherency or exclude remapping faults.

Intel-maintained e1000e regs.h identifies 0x3410/0x3418 as TDFH/TDFT (Tx
Data FIFO Head/Tail); the descriptor base registers are 0x3800/0x3804 and
length is 0x3808. The public I219 PHY datasheet does not supply these MAC
register definitions. FIFO indices should not equal the physical ring base;
0x900 is not evidence that TDBAL failed to propagate. The log now labels
this distinction explicitly. Source:
https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/net/ethernet/intel/e1000e/regs.h

### Frame-buffer readback and GCR/IOSFPC reference comparison

The next diagnostic build copies up to 60 bytes from the allocated TX page
through vmm_phys_to_virt(s_tx_pages[slot]), after the diagnostic SFENCE and
before TDT. For the 60-byte opt-in self-test this covers every byte. It compares
the copy with the submitted frame and prints MATCH/MISMATCH plus four hex rows
outside the device lock. It does not hardcode the observed 0x0424D000 address.
Descriptor/ring/errata programming and containment are unchanged.

This function already uses the same HHDM mapping to write the frame, so the
readback does not observe a separate physical alias or prove DMA visibility.
A fault in a CPU mapping would not by itself establish whether the NIC can
access that physical page. An identical readback does not prove a PCH-only
fault or rule out DMA remapping/coherency problems. The FIFO increment of two
is a change in the internal pointer value, not a demonstrated two-byte transfer.

Register-name correction: 0x1000 is PBA, 0x1008 is PBS. GCR is 0x5B00 and
IOSFPC is 0xF28. The existing Dell dump already contains GCR=0 and
IOSFPC=0x01011108. The new output labels and compares their managed fields:
GCR no-snoop bits 5:0 are clear, matching the PCH snoop policy; IOSFPC bit 16
is set, matching the SPT workaround in e1000_configure_tx. These are not
identified as a generic TX datapath-enable bit.

Full-register qualification: Linux e1000_init_hw_ich8lan passes
(uint32_t)~PCIE_NO_SNOOP_ALL (0xFFFFFFC0) to e1000e_set_pcie_no_snoop;
the helper clears low six bits then ORs that value. FortressOS currently
clears only the low six bits, preserving the upper bits. Thus GCR=0 matches
the low-six-bit policy, not the complete Linux write value. The significance
or writability of upper bits is not established by this log; no speculative
upper-bit write was added under this diagnostics-only request.

Primary references: Linux v6.12 e1000e regs.h (offsets), defines.h
(PCIE_NO_SNOOP_ALL and RDMTS_HEX), ich8lan.c (init_hw snoop policy), mac.c
(set_pcie_no_snoop), and netdev.c (SPT IOSFPC workaround), linked above.

### PCI error / VT-d snapshots and measured polling budget

The frame-readback Dell run matches all 60 submitted bytes, yet still lacks
TX DD. That validates the CPU copy, not DMA visibility. Implemented next:

- Read-only PCI STATUS (including error mask 0xF900), PM capability PMCSR
  and D-state, PCIe Device Status, and AER uncorrectable/correctable status.
  Capability walks validate bounds/alignment and terminate on malformed or
  cyclic lists. Extended configuration is attempted only with MCFG; the PCI
  API rejects extended reads without matching ECAM coverage. No W1C status
  writes or AER configuration changes occur.
- Exact boot opt-in prepares DMAR diagnostics outside all locks. Validate
  checksum/table/entry lengths, inspect at most four DRHD units, map only
  required MMIO pages at 0xFFFFFFFFE2100000 + index*64KiB using UC/NX read-only
  mappings. Retain mappings until reboot, including any partial failure.
  Read CAP-derived fault-register ranges after validating bounds; copy GSTS,
  FSTS and PMEN before TDT and before containment. Capture the first pending
  fault selected by FSTS.FRI when its valid high word is stable across reads;
  print raw 128 bits, reason, SID and whether segment/SID match the NIC BDF.
  No VT-d register, root/context/PASID table, fault-clear, translation-enable
  or interrupt configuration writes occur. DRHD device-scope routing is not
  resolved; all discovered units are labelled as inventory. This is not an
  IOMMU driver, and absence of DMAR or a sampled fault is not proof that DMA
  access is unrestricted. Advanced faults and all pending records are not
  exhaustively audited; sampled records can predate this TX attempt.
- PCH TX now uses at most 100 successful PIT-channel-2 1ms intervals, with
  DD checked before each wait and after the final interval. Waits run outside
  the device lock, require no interrupts or scheduler ticks, and stop early
  on timer failure. Print interval count on completion/timeout. This is a
  measured PIT wait budget, not an exact 100ms wall-clock deadline: initial
  diagnostic printing and CPU overhead are excluded. Boot polling remains
  the supported PCH use; no concurrent PIT-channel users or worker lifecycle
  are introduced. QEMU legacy TX retains its existing bounded polling loop.

The fatal containment function and descriptor/ring/errata settings remain
unchanged. Diagnostics copy under the device lock and print after unlocking;
timeout reporting prints retained snapshots after containment. New internal
helper: src/drivers/e1000_diag.h. References: Intel VT-d architecture spec
https://cdrdv2-public.intel.com/831418/vt-directed-io-spec.pdf and Linux v6.12
https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/iommu/intel/iommu.h
(register offsets, CAP.FRO/NFR), include/acpi/actbl1.h (DMAR/DRHD layout).

Verification: test-net-i219-host sanitizer checks cover 90ms completion,
100ms timeout, early timer failure, matching fault SID, unchanged mock VT-d
register contents, missing/bad-checksum/malformed/map-failed DMAR, PCI PM/PCIe/
AER extraction and bounded capability cycles. test-net-rings-host PASS; four
BIOS/UEFI x e1000/e1000e rings cases PASS. No Dell/real-IOMMU validation claimed.

### RX-first experiment and FWSM interpretation

The latest Dell report still has no TX DD after the 100ms polling budget.
Both reported DRHD units have translation disabled and PCI STATUS error bits
are clear; AER was unavailable. The next physical experiment is pending.

`net_test=rings net_rx_first=1` selects an I219-only, RX-only observation
window of at most 30,000 successful PIT 1ms intervals. It records the first
descriptor DD/status/errors/length and initial/final RDH, consumes and recycles
received packets, and recognizes the peer's exact 60-byte test payload.
It skips local TX and the PCI/VT-d diagnostic preparation. No new register
dump or initialization write is added. Missing traffic is inconclusive and
does not invoke fatal containment; an existing fatal condition still uses
the unchanged containment path. The window ends early on timer failure.

On a Linux wired peer, start before booting the Dell:

```sh
sudo python3 scripts/send_net_2b_rx.py YOUR_WIRED_INTERFACE
```

The helper submits ten frames per second for 90 seconds, addressed to
C8:F7:50:0E:35:80 with EtherType 0x88B5, marker FORTRESS-NET-2B-RX and 28
0xA5 bytes. Use the raw image's Verbose Debug entry, or the ISO default entry.
`RX PASS` proves this receive path completed and delivered the expected frame;
DD/RDH movement without a matching frame proves some receive progress.
No DD does not distinguish DMA failure from absent/filtered peer traffic.

After the RX window, decode the single FWSM read at 0x5B54. Linux v6.12
defines.h, ich8lan.h and manage.h identify E001C25C as FW_VALID=1,
RSPCIPHY=1, WLOCK_MAC=4, ULP_CFG_DONE=0, PCIM2PCI=0 and raw MODE=6.
RSPCIPHY describes PHY reset behavior on PCI reset, not active TX permission.
WLOCK_MAC=4 leaves RAR0 and SHRA0..3 available in the LPT reference.
MODE=6 and other high bits are not assigned speculative meanings.
FWSM is firmware status, not DMA clock/link control. The DMA clock-gating bit
is defined in CTRL_EXT (0x00080000). No FWSM enable write follows this decode.

Verification: test-net-i219-host ASan/UBSan PASS (matching RX, errored RX,
no traffic, recycling, bounded window, no TX or DMAR preparation);
test-net-rings-host PASS; BIOS/UEFI x e1000/e1000e rings 4/4 PASS.
Physical RX, Dell TX and cable capture remain pending.

### Dell receive progress and SPT link-up reference subset

User-provided Dell RX-only log: first descriptor status=3 (DD|EOP),
errors=0, length=60; RDH 0->3, 67 observed descriptor completions in the
30,000ms window. The exact peer marker did not match. This establishes
receive DMA progress on physical hardware, not an exact-frame RX PASS,
TX DMA reachability or successful TX. FWSM remains E001C25C.

Implemented next candidate for 8086:15D7 only: the link-up subset of Linux
v6.12 `e1000_check_for_copper_link_ich8lan`, using bounded MDIC transactions
under EXTCNF_CTRL.SWFLAG. At 1Gb/s, PHY page772/reg28 clock-gating interval
is 0xFA (preserve bits outside 0x7FF), page770/reg17 requests the K1 clock
(bit9), and page776/reg20 pointer gap bits11:2 are raised to at least0x18.
At 10/100Mb/s, interval=0x3E8, no gigabit K1 request is added and the
reference page776/reg20 value is0xC023. Existing negotiation is preserved.
MDIC uses PHY address1 and page selector31=page*32 per the HV access helper.
Each command has at most ten PIT1ms polls, checks READY/ERROR and returned
register/address, and waits another1ms after completion. Changed registers
are read back. Save/restore the original PHY page and release SWFLAG on
completed-command failures; an uncompleted command is not overwritten by a
restore command. Failure stops before ring allocation/mastering activation,
using the unchanged containment path; no PHY reset/restart or SMBus override.

Also match the reference MAC link-up fields: SLU set, forced speed/duplex
clear, FEXTNVM4 beacon duration low3bits=7 (8us), and SPT FEXTNVM6 bit31
copied from PCIEANACFG bit31. No new MMIO dump was added. These are reference
steps, not a demonstrated explanation for the Dell TX stall. Remaining full
PHY reset/D0/ULP/EEE/LTR/EMI initialization is not implemented by this subset.

The next ISO default/raw-image Verbose Debug entry selects
`net_test=rings net_tx_trial=1`: one TX attempt without the earlier
descriptor/frame/MMIO/PCI/VT-d diagnostic probes. Timeout retains the existing
fatal snapshot/reset/mastering-off/DMA quarantine. RX-only mode remains
available with `net_rx_first=1`. Physical TX and cable capture remain pending.

References:
[link-up sequence](https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/intel/e1000e/ich8lan.c),
[HV MDIC/page access](https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/intel/e1000e/phy.c),
[PHY field definitions](https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/intel/e1000e/ich8lan.h).

Validation: `make test-net-i219-host` ASan/UBSan PASS for gigabit/100Mb/s,
preserved larger gap/unrelated PHY bits, page/ownership restore, MDIC timeout,
ERROR and failed readback, and TX trial without old probes. Existing
`make test-net-rings-host` PASS and `make test-net-rings` 4/4 BIOS/UEFI x
e1000/e1000e PASS. `make` rebuilt ISO/raw image; GPT/FAT32/ext2 verification
PASS. This build has not yet been booted on the Dell.

### TIPG checkpoints after unsuccessful PHY-subset trial

User reports TX still stalls after the SPT PHY subset. Add TIPG(0x410),
TCTL(0x400), TXDCTL0(0x3828) to both PRE-TDT and timeout checkpoints.
The isolated trial prints only these three registers and decoded TIPG fields;
the timeout copy is captured before the unchanged containment resets the MAC.
No repeated PCI/VT-d or descriptor probes are added. Full legacy diagnostic
checkpoints also include these fields. RX and PHY setup remain unchanged.

The existing gigabit I219 write is0x00602008: IPGT8/IPGR1=8/IPGR2=6,
matching the Linux PCH link-up reference. 0x00602006 has IPGT6, not10;
10 would encode as0x0060200A. There is no evidence yet of actual TIPG=0x900;
the earlier log's0x900 came from FIFO pointers at0x3410/0x3418. Do not replace
the reference gigabit value with the misdecoded suggested constant.

0x20 is MDIC, not PHY/MAC datapath control. 0x143F6100 decodes as READY=1,
ERROR=0, WRITE operation, PHY address1, register31 and data0x6100. The low
16bits are transaction data:0x6100=776*32, a PHY page-selector value.
The previous0x6080=772*32 selected page772. Bit8 here is part of the page
value, not a TX-enable bit. The complete values differ by0x180 (bit7 cleared,
bit8 set), not only bit8. MDIC retains the last transaction; this is not an
independent live page-selector readback. Reference defines.h MDIC fields and
phy.c HV page access establish this interpretation.

### Descriptor-pair, TARC0 and TX page diagnostics

User reports correct TIPG=0x00602008 on the Dell, with TX still stalled.
Next trial captures all16 bytes of descriptors0 and1 through the ring's
physical-derived HHDM CPU mapping before TDT. This is CPU-visible memory,
not an observation of NIC reads. With TDT=1, only descriptor0 is published;
descriptor1 is an unused next slot. No descriptor is changed by diagnostics.

0x3840 is TARC0 (transmit arbitration), not a frame-DMA byte counter.
For0x2D800403, bits29:28 select0x20000000, matching SPT's two-request
workaround, and bits23/24/26/27 equal required mask0x0D800000. Other bits
are preserved; the cited e1000e code does not name all of them. Report these
fields alongside both TIPG/TCTL/TXDCTL checkpoints without modifying TARC0.
TDFT0x900->0x902 is internal FIFO pointer movement: it does not demonstrate
that two bytes were read from the frame or locate a DMA stall at byte2.

The TX buffer is one exclusively retained PMM allocation; initialization
already zeroes the whole4096-byte page, and transmit copies the frame into
that same HHDM mapping. A read-only boot-time kernel page-table walk now
reports all walked entries, leaf level, resolved page-start/end physical
addresses and the leaf PAT index (PWT/PCD/PAT). It handles4KiB/2MiB/1GiB
leaves and absent entries, with no mapping/cache/PAT/MTRR changes. HHDM
kernel mappings are static at this boot checkpoint; no VMM ownership change
or new public VMM API is introduced. Raw entries expose present/RW/NX flags.
CPU page-table flags govern CPU access, not NIC DMA permissions. The PAT
index alone does not establish the effective memory type without PAT/MTRRs.
Physical address matching and CPU readback remain insufficient to prove
NIC visibility; missing CPU mapping is reported rather than dereferenced.

Host sanitizer tests verify descriptor-pair/TARC output and4KiB first/last,
absent and2MiB mapping walks. RX, containment and all initialization steps
remain unchanged. Physical TX acceptance is still pending.

### Working Linux Mint baseline: preserve post-reset TCTL

User confirms Linux Mint kernel7.0.0-31-generic/e1000e on the same
0000:00:1f.6 Dell transmits successfully; firmware-version0.1-4.
Its reported TCTL=0x3103F0FA differs from FortressOS0x0103F0FA in bits28/29.
Upstream Linuxv7.0 configure_tx reads TCTL, modifies CT/PSP/RTLC, and
config_collision_dist modifies COLD; it does not construct TCTL from zero.
Upstream is a reference, not proof of the exact Ubuntu/Mint patch contents.

Correct FortressOS's ring-init loss of post-reset bits: save TCTL before the
engine-disable write, preserve bits outside EN/CT/COLD and set the documented
TX fields. TARC1 bit28 is cleared when retained TCTL.MULR(bit28) is set,
and set otherwise, matching initialize_hw_bits. Bit29 is preserved only if
present in post-reset hardware state; its function is not guessed or forced.
Print post-reset/final TCTL and add TARC1 to the existing checkpoints.
RX, PHY, descriptors, ring size, TIDV and fatal containment are unchanged.
This is a supported initialization correction, not a confirmed TX fix.
Host tests cover retained bits28/29 and consistent TARC1, alongside existing
zero-default configuration and quarantine tests. Physical retest pending.

[Linuxv7.0 TX configuration](https://github.com/torvalds/linux/blob/v7.0/drivers/net/ethernet/intel/e1000e/netdev.c),
[TARC initialization](https://github.com/torvalds/linux/blob/v7.0/drivers/net/ethernet/intel/e1000e/ich8lan.c).
