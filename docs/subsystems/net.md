# Networking Subsystem Annex

This annex documents the current status, hardware facts, verification evidence, and scope boundaries of the FortressOS networking subsystem. Binding contracts, locking rules (L1–L4), memory ownership (M1, M4), and interrupt invariants (I1–I3) live in [`AGENTS.md`](../../AGENTS.md) (§4 and §9). Architectural specifications and roadmap milestones live in [`docs/plans/NET_PLAN.md`](../plans/NET_PLAN.md).

---

## 1. Subsystem Status and Overview

Phase 4a IPv4/ICMP and Ring 3 `/bin/ping` are implemented; physical Phase 4b is
pending. [Phase 4a](../roadmap/net-phase4a.md) records the gates and limitations;
[NETCTL_PING ABI](../plans/NETCTL_PING_ABI.md) specifies the 48-byte layout and
errors. Ring 3 callers use a bounded mailbox; the BSP worker remains the sole
protocol owner. No scheduler/signal/driver changes or new timer hook were needed.
Hard-exit/STOP cleanup is bounded by a finite lease, with stale-token protection.
Earlier Phase 3/2b checkpoint descriptions below are historical evidence.

Phase 3 is implemented (2026-10-01): Ethernet/ARP dispatch, bounded `net=`
configuration and a BSP-pinned, tick-sleeping ingress worker. The user authorized
one additional BSP timer wake target alongside the existing input tick; the
scheduler and lock contracts are unchanged. Verification and boundaries:
[Phase 3](../roadmap/net-phase3.md). The table below retains the detailed Phase
2b hardware checkpoint; see the Phase 4a update above.

Phase 3 historical checkpoint (`src/net/net.c`/`net.h`): single-interface Ethernet dispatch
(bounds-checked decode, destination-MAC filter, ARP-only handling; IPv4 and all
other EtherTypes drop). ARP replies `who-has` requests for the configured local
IP and learns **replies only** — request senders are not cached; `arp_resolve()`
is nonblocking (0 hit, 1 request submitted, −1 invalid/send failure). Every
non-null RX packet from a valid device callback is recycled **exactly once**,
including malformed and send-failure paths; no DMA allocation is freed. The
`net=` parser is bounded (at most 512 bytes, prefix 1–30, defaults
`10.0.2.15/24,10.0.2.2`) and warns then falls back on malformed or duplicate
tokens without panicking. Codec byte order: decoded EtherType and ARP opcode are
**host-order** (callers compare raw constants); IPv4 addresses stay
**network-order**. The BSP-pinned worker drains at most 64 packets per iteration
and, when idle or partial, sets `s_deadline = apic_timer_get_bsp_ticks() + 1` and
calls `sched_wait_until(&g_net_poll_channel, deadline_reached, &s_deadline)`; a
full batch `thread_yield()`s for fairness, and there is no idle yield loop. The
worker starts after the raw selftest; `net_test=rings` skips worker publication to
preserve exclusive raw-test RX ownership (an absent device or failed creation also
skips publish). The BSP timer hook `net_timer_tick()` runs alongside
`input_timer_tick()` in `apic_timer_handler` and, once the worker is published,
calls `sched_wake_all(&g_net_poll_channel)` — a stable global address, allocating
and logging nothing and holding no device lock. No scheduler, lock-rank, EOI or
`sched_wait_until` signature changed. These stack APIs are **single-BSP-owner and
non-reentrant** (BSS-static state, ARP cache and TX scratch); cross-core socket
callers and their synchronization remain Phase 5 work; Phase 4a uses the BSP mailbox.

| Subsystem | Status | Detail |
| --- | --- | --- |
| **NET** | **IN PROGRESS** — Phase 4a complete (2026-10-01); physical Phase 4b pending. | Phases 0–3 are complete; see [Phase 3](../roadmap/net-phase3.md) for Ethernet/ARP and tick-sleeping worker verification. Phase 2b I219 SPT/CNP MAC takeover/DMA and the bounded fatal stop-report are implemented and **physically accepted on the Dell Latitude 5590** (2026-09-30, manual observation + cable-side capture): RX works (`DD`/`EOP` set, `errors=0`, `length=60`, `RDH` 0→3) and TX completes (`[NET 2b] TX PASS: 60-byte 88B5 frame, DD observed`), with that frame independently captured on the wire by a second host running Wireshark (frame #437: source `Dell_0e:35:80` / `C8:F7:50:0E:35:80`, broadcast, EtherType `0x88b5`, length 60). Root cause of the earlier TX stall: the upper `TCTL` bits were not preserved post-reset (FortressOS `0x0103F0FA` vs. the working Linux e1000e `0x3103F0FA`); preserving the post-reset bits fixed it (`TCTL post-reset/final=3003F0F8/3103F0FA`), alongside `FEXTNVM11` bit 13 (reset-hang erratum) before enabling the rings, `CTRL_EXT`/`TARC`/`IOSFPC` SPT workarounds, and PHY link-up configuration (PLL/K1/FIFO gap) with `TARC1` kept consistent with `TCTL.MULR`. `make test-net-rings` passes 4/4 BIOS/UEFI × e1000/e1000e at SMP=1 with exact 60-byte TX pcap and injected RX checks; `make test-net-rings-host` covers ownership/failure paths with ASan/UBSan hardware mocks. I219 `15D7`/`15BD`/`15BB` take a separate PCH init/reset path; older `15B7`/`156F` stay discovery-only. `make test-net-i219-host` passes mocked sanitizer coverage; it makes no physical DMA claim. Details: [Phase 2a](../roadmap/net-phase2a.md), [Phase 2b](../roadmap/net-phase2b.md). Pure host-testable foundation (net_dev_t, pbuf_t, RFC 1071 checksum, eth/arp/ipv4 codecs; 103/103 tests pass with ASan/UBSan via `make test-net-host`), plus Intel e1000/e1000e PCI discovery, uncached MMIO mapping (PCD/PWT/NX), MAC address and link STATUS read verified in QEMU across BIOS and UEFI present/absent cases (`make test-net-pci`). The **Dell Latitude 5590 is the NET acceptance machine**: manual observation (2026-09-30) confirmed I219-LM `8086:15D7` at `0000:00:1F.6`, BAR0 `0xEF300000` mapped to `0xFFFFFFFFE2000000`, MAC `C8:F7:50:0E:35:80`, and PHY link-up (`STATUS 0x00080083`), so the `NET_PLAN.md` §7.2 PHY/CSME risk is not blocking there; Phase 1b has no automated target and its evidence is the manual boot-log photos, not a test pass, and 2b's acceptance is likewise a manual observation plus a cable-side capture, not a test pass. The 2b boundary is deliberately narrow: **one raw frame and one cable-side capture per boot — no cross-core execution, sustained traffic, worker cadence or eth/ARP/IPv4 protocol delivery is claimed.** Details: [Phase 0](../roadmap/net-phase0.md), [Phase 1a](../roadmap/net-phase1a.md), [Phase 1b](../roadmap/net-phase1b.md). |

---

## 2. Hardware Facts and Verification Boundaries

| ID | Evidence / constraint |
| --- | --- |
| H13 | **Dell 5590 boot-log photos + user report (2026-09-30, NET Phase 1b):** integrated NIC is Intel I219-LM at `0000:00:1F.6`, PCI `8086:15D7`, class `02:00:00`, BAR0 `0xEF300000` (32-bit MMIO) mapped to `0xFFFFFFFFE2000000`, MAC `C8:F7:50:0E:35:80`. `STATUS` read `0x40080000` with no cable (LU clear) and `0x00080083` with a cable attached (FD + LU + 1000 Mb/s per `e1000.h` bits), i.e. **PHY/CSME link negotiation completes on this machine**, so the `NET_PLAN.md` §7.2 Phase 2b stop-condition did not trigger here — keep that stop-condition anyway; it is untested on other units/firmware. `0x15D7` is the ID actually observed for this 5590, not the `0x15B7` entry the pre-1b header labelled "Dell Latitude 5590 physical". Observations of one machine: never hardcode the BDF, BAR0 base/aperture, or MAC. **Phase 2b code (2026-09-30):** SPT/CNP MAC-only takeover, DMA settings and bounded fatal diagnostic/quarantine path are implemented and host-mock tested; the user reports a Dell polling-DMA-ready boot followed by TX timeout (TDH=0/TDT=1), with fatal containment; HTHRESH=0 was corrected to 1, but the next user-supplied Dell log confirms the correct physical 60-byte descriptor and TXDCTL=0x0141011F with TDH still zero. A later checkpoint shows TX FIFO tail movement and 14 KiB TX allocation. Post-reset restoration/readback of FEXTNVM11 bit 13 was added following iPXE startup policy; Read-only PCI/VT-d snapshots and a 100ms PIT TX wait budget are implemented and host tested. **Physical acceptance (2026-09-30, manual observation + cable-side capture):** RX works (`DD`/`EOP` set, `errors=0`, `length=60`, `RDH` advanced 0→3) and TX completes (`[NET 2b] TX PASS: 60-byte 88B5 frame, DD observed`); the 60-byte `0x88b5` broadcast frame was independently captured on the wire by a second host running Wireshark (frame #437: source `Dell_0e:35:80`, i.e. `C8:F7:50:0E:35:80`). Root cause of the TX stall was the loss of the upper `TCTL` bits: FortressOS programmed `TCTL = 0x0103F0FA` while the working Linux e1000e baseline leaves `TCTL = 0x3103F0FA`; preserving the post-reset bits resolved the stall (`TCTL post-reset/final=3003F0F8/3103F0FA`). Also applied per the I219/SPT reference: `FEXTNVM11` bit 13 (reset-hang erratum) before enabling the rings, the `CTRL_EXT`/`TARC`/`IOSFPC` SPT workarounds, PHY link-up configuration (PLL/K1/FIFO gap), and `TARC1` kept consistent with `TCTL.MULR`. Method: direct MMIO dumps were blocked (`/dev/mem` locked — `devmem2` and an mmap helper both returned "Operation not permitted"), so the working register baseline came from this machine's own Linux Mint install via `ethtool -d enp0s31f6`, which dumps the I219 registers through the e1000e driver; the `TCTL` difference was visible there. The bounded fatal-stop containment path is unchanged and was available but not triggered on the final run. Boundary: one raw frame and one cable-side capture per boot — no sustained traffic, cross-core execution, worker cadence or protocol (eth/ARP/IPv4) delivery is claimed. Full PHY/CSME recovery is not implemented; failed link/ownership/reset stops networking. See [Phase 2b](../roadmap/net-phase2b.md). |

---

## 3. Physical Hardware Acceptance

### Dell bare-metal acceptance: NET Phase 2b (2026-09-30)

User-supplied testing confirms physical bare-metal networking on the NET acceptance machine, a Dell Latitude 5590 booted via UEFI from USB, with the integrated I219-LM (`8086:15D7` at `0000:00:1F.6`, MAC `C8:F7:50:0E:35:80`) cabled to the same LAN as a second host:

- **RX (manual observation):** a received descriptor completed with `DD` and `EOP` set, `errors=0`, `length=60`, and `RDH` advanced 0→3.
- **TX (manual observation):** `[NET 2b] TX PASS: 60-byte 88B5 frame, DD observed` — the transmit descriptor completed (DD) with no fatal stop; the bounded containment path was available but not triggered.
- **Wire capture (independent, cable-side):** Wireshark on the second host captured the frame as #437 — source `Dell_0e:35:80` (`C8:F7:50:0E:35:80`), broadcast destination, EtherType `0x88b5`, length 60, payload `FORTRESS-NET-2B-TX` followed by 28 `0xA5` bytes.
- **Root cause and fixes:** see H13 — the upper `TCTL` bits were not preserved across reset (`0x0103F0FA` vs. the working Linux e1000e `0x3103F0FA`), plus the `FEXTNVM11` bit 13, SPT and PHY link-up work.
- **Method note:** direct MMIO reads of the live controller were blocked (`/dev/mem` locked, `Operation not permitted` for both `devmem2` and an mmap helper), so the working register baseline was taken from the machine's own Linux Mint install via `ethtool -d enp0s31f6` through the e1000e driver.

Evidence boundary: this is a **manual observation plus a cable-side capture**, not an automated test pass. It establishes one raw 60-byte frame received and transmitted on one boot; it does not claim sustained traffic, cross-core execution, worker cadence, or eth/ARP/IPv4 protocol delivery. `make test-net-rings` (4/4 BIOS/UEFI × e1000/e1000e) remains the automated logic evidence. Recorded in [docs/roadmap/net-phase2b.md](../roadmap/net-phase2b.md).

---

## 4. Test Targets and Verification Notes

Phase 4a verification: `make test-net-icmp-host`, `make test-net-ipv4-host`,
and `make test-net-ping-host` PASS under ASan/UBSan; `make test-net-icmp` PASS
8/8 BIOS/UEFI × e1000/e1000e × user/socket, SMP=1. Real Ring 3 ping and ABI
pointer fixture, independent pcap audit, pending/timeout recovery, concurrent
busy, killed-owner lease recovery and one STOP/CONT expiry case are covered.
All QEMU fixtures are disposable with no data disks. Physical ICMP and idle CPU
measurements are not claimed. Full commands and boundaries:
[Phase 4a evidence](../roadmap/net-phase4a.md).

| Target | Scope / evidence |
| --- | --- |
| `make test-net-host` | NET Phase 0 host ASan/UBSan: RFC 1071 ones' complement checksum vectors (odd/even lengths, bounds, multi-buffer accumulation), Ethernet II (encode/decode, bounds 60-1514B, runt/oversize rejection, broadcast/MAC filter), ARP (encode/decode request/reply, truncation rejection, 16-entry bounded cache stub), IPv4 (encode/decode, checksum verify/corrupt, fragment rejection, bounds, malformed IHL/len), pbuf_t lifecycle and fuzz/bounds resilience. 103/103 PASS 2026-09-30. |
| `make test-net-eth-host` | NET Phase 3 host ASan/UBSan: actual `net.c` stack with `eth`/`arp` codecs and a mocked NIC, scheduler and BSP ticks. Every input recycled exactly once; ARP request replies and reply-only cache learning; malformed/truncated/oversize/non-matching frames dropped; send-failure and `arp_resolve` outcomes; bounded `net=` config (defaults, prefix 1–30, duplicate/malformed fallback); absent device, failed worker creation and `net_test=rings` raw-test exclusion; three mock idle waits proving the tick-deadline predicate and channel wake with no idle yield. PASS 2026-10-01; no hardware/IRQ/real-scheduler/idle-CPU claim. |
| `make test-net-eth` | NET Phase 3 QEMU, SMP=1, BIOS/UEFI × e1000/e1000e × {user, socket} = 8 cases; disposable ISO/OVMF vars, no data disks, exact argv preflight. User backend resolves the real SLIRP gateway; socket backend emulates the gateway reply and, after idle ticks, injects a peer ARP request over loopback UDP to prove the sleeping worker resumes to poll RX. Guest request/reply audited as exact 60-byte outbound pcap records; RX injection is loopback UDP, never `filter-dump`; real Ring 3 shell prompt confirmed while the worker runs. 8/8 PASS 2026-10-01; no physical acceptance, sustained load, measured idle CPU or cross-core delivery claim. |
| `make test-net-rings` | NET Phase 2a: BIOS/UEFI × e1000/e1000e SMP=1, disposable ISO/OVMF vars, no data disks, exact argv preflight; raw TX DD + 60-byte pcap audit, injected raw RX byte check + recycle, shell ready. Includes host ownership/failure sanitizer target. 4/4 PASS 2026-09-30; no physical DMA or worker claim. |
| `make test-net-rings-host` | Actual e1000 driver with mocked PMM/MMIO/PCI/pthread locks under ASan/UBSan: allocation failures, RX pool/recycle/wrap/malformed frames, TX copy/reservation/wrap, completion/reset timeout quarantine. PASS 2026-09-30; no hardware timing/IRQ claim. |
| `make test-net-pci` | NET Phase 1a QEMU BIOS + UEFI SMP=1: Intel e1000/e1000e PCI discovery (8086:100E/10D3), BAR0 sizing & uncached MMIO mapping (`0xFFFFFFFFE2000000ULL`), valid hardware MAC read (`RAL0`/`RAH0` and `EERD`), link STATUS register, clean fallback report on absent NIC (`-net none`), and argv preflight rejecting unauthorized storage. 4/4 PASS 2026-09-30. |
| `make test-net-i219-host` | Mocked I219 sanitizer coverage for register and reset flows; makes no physical DMA claim. |

### Deliberate Absence of Automated Targets

- **NET Phase 1b (Dell hardware discovery)** deliberately adds **no** target: it is a manual hardware observation recorded in [`docs/roadmap/net-phase1b.md`](../roadmap/net-phase1b.md) and must never be reported as a test pass. `make test-net-pci` stays the primary logic evidence for the driver's discovery/mapping/MAC/STATUS path.
- **NET Phase 2b (Dell I219-LM DMA/TX/RX)** also deliberately adds **no** target: its physical acceptance is a manual hardware observation plus a cable-side capture on a second host, recorded in [`docs/roadmap/net-phase2b.md`](../roadmap/net-phase2b.md), and must never be reported as a test pass. `make test-net-rings` (4/4 BIOS/UEFI × e1000/e1000e) and `make test-net-rings-host` remain the automated logic evidence for the rings/DMA/TX/RX and ownership/failure paths; `make test-net-i219-host` is mocked-only and makes no physical DMA claim.

---

## 5. Architectural References

- Master Specification: [`docs/plans/NET_PLAN.md`](../plans/NET_PLAN.md)
- Phase 0 Implementation & Verification: [`docs/roadmap/net-phase0.md`](../roadmap/net-phase0.md)
- Phase 1a PCI Discovery & MMIO Mapping: [`docs/roadmap/net-phase1a.md`](../roadmap/net-phase1a.md)
- Phase 1b Physical Dell Discovery: [`docs/roadmap/net-phase1b.md`](../roadmap/net-phase1b.md)
- Phase 2a QEMU Ring & Descriptor Verification: [`docs/roadmap/net-phase2a.md`](../roadmap/net-phase2a.md)
- Phase 2b Physical Dell I219-LM DMA & Capture: [`docs/roadmap/net-phase2b.md`](../roadmap/net-phase2b.md)
- Phase 3 Ethernet/ARP and BSP ingress worker: [`docs/roadmap/net-phase3.md`](../roadmap/net-phase3.md)
