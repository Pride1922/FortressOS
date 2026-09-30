# Networking Phase 1b — Dell Latitude 5590 Hardware Discovery

**Status**: COMPLETE (2026-09-30), manual hardware observation.
**Deliverables**: `src/drivers/e1000.h`, `src/drivers/e1000.c` (supported device-ID list
only). No automated target covers this phase.
**Verification**:
- Manual user observation on a Dell Latitude 5590 booted from USB; evidence is the
  user's report and boot-log photos (2026-09-30).
- Phase 1a's `make test-net-pci` remains the primary, automated logic evidence for the
  same discovery/mapping/MAC/STATUS path, and Phase 1b does not replace or extend it.
- No DMA, no descriptor rings, no interrupts and no packet I/O were exercised; this
  phase is discovery only, exactly like Phase 1a but on physical hardware.

---

## 1. Scope

Phase 1b is the physical-hardware checkpoint of the Phase 1 driver bring-up
(`docs/plans/NET_PLAN.md` §8: "Phase 1b — Dell Hardware Discovery"). It runs the Phase 1a
code path unchanged — PCI scan, BAR0 validation/sizing, uncached MMIO mapping, `RAL0`/`RAH0`
MAC read, `STATUS` read — on the project's primary hardware verification target, and
records what that machine actually reports.

Explicitly not in scope: descriptor ring allocation, bus mastering, MSI/IOAPIC routing,
raw frame transmit/receive (Phase 2a/2b), and any protocol logic (Phase 3+).

---

## 2. Observed values (Dell Latitude 5590)

| Field | Observed |
| --- | --- |
| Machine | Dell Latitude 5590 |
| NIC | Intel I219-LM |
| PCI vendor:device | `8086:15D7` |
| BDF | `0000:00:1F.6` |
| PCI class | `02:00:00` |
| BAR0 | `0xEF300000`, 32-bit MMIO |
| Mapped virtual window | `0xFFFFFFFFE2000000ULL` (uncached: `PTE_PCD \| PTE_PWT \| PTE_NX`) |
| MAC | `C8:F7:50:0E:35:80` |
| `STATUS`, no cable | `0x40080000` — link down (`LU` clear) |
| `STATUS`, cable connected | `0x00080083` — link up (`FD` + `LU` + 1000 Mb/s per `e1000.h` bit definitions) |

Result: **discovery, BAR0 mapping, MAC read and PHY link-up all verified on physical
hardware.**

Notes on the raw values, stated as observations rather than decoded facts:

- The no-cable value differs from the cable value in bit 30 (`0x40000000`), which has no
  documented meaning in `e1000.h` and was not investigated here. Bit 19 (`0x00080000`) is
  set in both reads. Neither is promoted to a driver interpretation.
- `0x00080083` sets the driver's existing `E1000_STATUS_FD`, `E1000_STATUS_LU` and
  `E1000_STATUS_SPEED_1000` bits, and `LU` is what Phase 2a/2b would sample.
- These are observations of **one machine**. Never hardcode the BDF, the BAR0 base
  address, the aperture size or the MAC address; see H13 in `AGENTS.md` §8.

---

## 3. Device-ID addition

`src/drivers/e1000.h` gained three Intel I219-LM device IDs, and `is_supported_e1000()`
in `src/drivers/e1000.c` now accepts them:

| Macro | ID | Basis |
| --- | --- | --- |
| `E1000_DEV_I219_LM_15D7` | `0x15D7` | **Observed** on this Latitude 5590 (Phase 1b) |
| `E1000_DEV_I219_LM_15BD` | `0x15BD` | Declared for the Latitude 5500 in `NET_PLAN.md` §2.1; not yet observed here |
| `E1000_DEV_I219_LM_15BB` | `0x15BB` | I219-LM variant; not yet observed here |

No matching logic, BAR handling, register access or diagnostics were changed — this is a
supported-ID list extension only.

Discrepancy to preserve rather than "fix": `e1000.h` also carries
`E1000_DEV_I219_LM` (`0x15B7`) commented "Dell Latitude 5590 physical" from the pre-1b
driver. The ID actually observed on this 5590 is `0x15D7`, so that earlier comment is not
1b evidence for this machine. `0x15B7` and `0x156F` remain accepted; there is no evidence
either way for them here, so they are left in place.

---

## 4. PHY / link-negotiation finding (`NET_PLAN.md` §7.2)

`NET_PLAN.md` §7.2 flags a Phase 2b stop-condition: I219-LM parts on Sunrise Point /
Cannon Point platforms can be held behind PHY clock gating or CSME power state, in which
case `STATUS.LU` stays clear and bring-up must stop, capture a register diagnostic, and
mark networking unavailable rather than thrash.

Observed on this Latitude 5590: **the link negotiates.** With a cable attached the driver
reads `STATUS = 0x00080083`, i.e. `LU` set and a 1000 Mb/s indication, with no PHY reset,
`MDIC` sequence or CSME workaround attempted — Phase 1b does not implement any of them.
The §7.2 risk is therefore **not blocking on this machine**.

That finding does not retire the stop-condition: it is one observation on one unit with
the firmware as shipped, and Phase 2a/2b still own bounded link verification, the
diagnostic capture, and the `-ENETDOWN` behavior if `LU` is clear. Keep the §7.2
stop-condition as written until 2b records its own evidence.

---

## 5. Evidence boundary

- This is a **manual user observation of one physical machine**; the agent neither
  observed nor reproduced it. There is no transcript, no serial capture and no automated
  assertions — the evidence is the reported boot-log output and its photos.
- **QEMU remains the primary logic evidence** for this driver path. `make test-net-pci`
  (Phase 1a: BIOS/UEFI, NIC present/absent, `4/4 PASS` 2026-09-30) is what validates the
  detection, sizing, mapping, MAC-fallback and diagnostics logic; Phase 1b shows that the
  same path works on real I219-LM silicon and firmware.
- No QEMU pass stands in for physical acceptance, and this physical observation does not
  stand in for the automated suite.
- Unchanged and unverified by 1b: ring/DMA behavior, bus mastering, interrupts, transmit
  and receive, link-down *handling* (only link *state* was read), and the Latitude 5500.
- Artifacts: Dell boot-log photos, 2026-09-30.

---

## 6. Next

**Phase 2a (Rings & Raw Frame I/O, QEMU)** is next: allocate TX/RX descriptor rings, send
a raw frame and poll the RX ring, with the acceptance gate being a `-netdev dump` pcap
audit of the transmitted frame. Phase 2b then repeats link-up and raw-frame work on Dell
hardware under the §7.2 stop-condition.
