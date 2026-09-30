# Platform & Power Subsystem Annex (Phase 9C.5)

This annex documents the current status, hardware facts, verification evidence, and scope boundaries of the FortressOS platform, ACPI power, PCI configuration (ECAM/MCFG), and console diagnostics. Binding contracts, interrupt invariants (I1–I3), memory mappings (M1–M4), and protected initialization order live in [`AGENTS.md`](../../AGENTS.md) (§4 and §9).

---

## 1. Subsystem Status and Overview

| Subsystem | Status | Detail |
| --- | --- | --- |
| **Phase 9C.5 Power & layout** | **COMPLETE** | ACPI S5 shutdown/reset and US/AZERTY switching, verified on Dell 5590. Full detail: [`docs/roadmap/subsystems.md`](../roadmap/subsystems.md) ("Dell Latitude 5590 physical acceptance"). |

---

## 2. Hardware Facts and Verification Boundaries

| ID | Evidence / constraint |
| --- | --- |
| H3 | **5590 photo:** ECAM segment 0 buses 0..127, base `0xF0000000`. Parse MCFG, never assume this address/range or apply it to another Dell. See H12 for 5530 controller topology. |
| H7 | **Code:** ACPI FADT/DSDT S5 and reset fallbacks now exist (`power.c`); this is limited parsing, not a general AML interpreter. Port `0x604` is a QEMU mechanism. Physical ACPI S5 shutdown and multi-tier reset confirmed functional on Dell 5590. |

---

## 3. Physical Hardware Acceptance

### Dell Latitude 5590 physical acceptance (Power & Platform)

- **Power:** ACPI S5 shutdown and multi-tier reset confirmed functional on Dell Latitude 5590.
- **Visuals:** Limine splash wallpaper and kernel boot logo / emblem verified.
- **Early Diagnostics:** COM1 and cached framebuffer diagnostics operational before PMM/heap initialization.

---

## 4. Test Targets and Verification Notes

| Target | Scope / evidence |
| --- | --- |
| `make test-power` | QEMU shutdown/reboot command tests; physical ACPI S5 confirmed separately on Dell 5590 (see H7), not by this target |
| `make test-boot-diagnostics` | UEFI 8 GiB, no COM1; progress to PCI discovery and framebuffer capture |
| `make test-console` | Host ASan/UBSan: pixel output, wrapping, scrolling and bounds |

---

## 5. Architectural References

- Cross-Cutting Subsystem Notes: [`docs/roadmap/subsystems.md`](../roadmap/subsystems.md)
