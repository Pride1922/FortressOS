# Phase 9H — 32 GiB RAM Support

Status: COMPLETE (2026-09-20). See `AGENTS.md` status table for current
summary; this file holds the detailed implementation notes and evidence.
See `PROTECTED.md` — the two-stage PMM/VMM ordering this phase established
is a do-not-touch-without-discussion contract, and is the direct dependency
Piece 6 of `SMP_DESIGN.md` re-audits for multi-core bring-up.

2026-09-24 re-audit note: the Phase 5 checkout contained the 32 GiB bitmap
but not the ceiling/unlock implementation described below. The available Git
history did not establish the cause. Historical evidence remains unchanged;
[Piece 6A](smp-piece6-memory.md) adds the missing invariant and has separate,
currently pending verification.

FortressOS now uses the Dell 5590's full 32 GiB of installed RAM. Previously
the PMM bitmap was capped at 2 GiB (64 KiB bitmap); the kernel could not
allocate frames above that ceiling.

## What was built

- PMM bitmap `PMM_BITMAP_CAPACITY_BYTES` extended from 64 KiB to 1 MiB,
  covering 32 GiB of physical RAM.
- `pmm_high_memory_probe()` added as a diagnostic: allocates one frame above
  2/4/16/30 GiB via `pmm_alloc_page_above()`, writes a pattern through
  `vmm_phys_to_virt`, reads back, verifies. Temporary; removed after
  acceptance.
- Two-stage PMM init:
  - `pmm_init` initializes the bitmap for 32 GiB but sets
    `g_alloc_ceiling = 1 GiB`, restricting allocation to safe low memory
    during VMM construction.
  - `vmm_init` builds the kernel PML4, maps the HHDM for all 32 GiB of
    usable RAM, allocates page-table frames from the capped region (always
    reachable via Limine's HHDM), switches CR3, then calls
    `pmm_unlock_high_memory()`.
  - After the CR3 switch, `phys_to_virt` reaches any physical page, so PMM
    can hand out frames across the full 32 GiB.
- PMM audit message and the "memory map exceeds bitmap" warning now derive
  their numbers from `PMM_BITMAP_MAX_RAM_BYTES`.

## Why it was necessary

Limine's HHDM on the Dell 5590 covers only physical `[0, ~2.5 GiB)`.
Measured by direct read at `hhdm_offset + phys`:

```
[HHDM-PROBE] Limine HHDM offset: 0xFFFF800000000000
0x100000 -> 0x1
0x40000000 -> 0x0
0x60000000 -> 0x0
0x70000000 -> 0x0
0x78000000 -> 0x0
0x80000000 -> 0x26
CPU EXCEPTION KERNEL PANIC
Faulting Linear Address (CR2): 0xFFFF8000A0000000
```

`0x80000000` (2 GiB) succeeds; `0xA0000000` (2.5 GiB) raises #PF. Without
the two-stage init, the VMM's own page-table allocations would fault as
soon as PMM handed out a frame above Limine's coverage. The 1 GiB ceiling
keeps all early-boot allocations within Limine's window until the kernel's
own HHDM takes over.

## Evidence

Boot log (`/mnt/boot.log`, preserved on USB, 2026-09-20, Dell 5590):

```
[WARN] PMM: memory map reports RAM above bitmap capacity; clamping to 32 GiB
[ OK ] PMM initialized:
       Total Physical RAM:  32768 MiB (8388608 frames)
       Usable Free RAM:     31873 MiB (8159717 frames)
       Used/Reserved RAM:   894 MiB (228891 frames)
       Bitmap Location:     Phys 0x100000 (1024 KiB)
[ OK ] PMM audit passed (bitmap reserved, frame 0 guarded, 32 GiB capacity verified)
...
[ OK ] CR3 switch survived! Kernel running on independent 4-level page tables.
[PMM] High-memory allocation unlocked
[PROBE] PMM total: 32 GiB, free: 31 GiB
Allocated phys 0x80000000 — HHDM readback PASS
Allocated phys 0x100000000 — HHDM readback PASS
Allocated phys 0x400000000 — HHDM readback PASS
Allocated phys 0x780000000 — HHDM readback PASS
```

Memory map top range: `[0x100000000 - 0x82E7EC000] Type: Usable RAM
(30121904 KiB)`, ending at ~32.72 GiB. The bitmap covers 32 GiB; the last
0.72 GiB is clamped and warned.

## What this does NOT do

- The 0.72 GiB above the bitmap ceiling is reserved, not usable. Raising
  `PMM_BITMAP_CAPACITY_BYTES` to 2 MiB would cover 64 GiB and eliminate the
  clamp.
- No user-space API for requesting large memory. Process stacks remain
  4 KiB. This change is a prerequisite, not a feature.
- SMP initialization is unchanged. The two-stage ordering assumes
  bootstrap-CPU-only execution during `vmm_init` — see `SMP_DESIGN.md`
  Piece 6 for the planned re-audit of this assumption once AP bring-up is
  implemented.

## Hardware facts recorded during this phase

- **H5:** Limine's HHDM on the Dell 5590 covers physical `[0, ~2.5 GiB)`
  only. Measured by direct read at `hhdm_offset + phys`: `0x80000000`
  succeeds, `0xA0000000` raises #PF at CR2=`0xFFFF8000A0000000`. FortressOS
  works around this with a two-stage PMM: allocation is capped at 1 GiB
  until `vmm_init` loads the kernel PML4 with a full 32 GiB HHDM, then
  `pmm_unlock_high_memory()` clears the cap. PMM bitmap is 1 MiB (32 GiB
  coverage); the memory map's top range ends at `0x82E7EC000` (~32.72 GiB),
  which is clamped and warned. Verified on the Dell: `dmesg` reports
  `Total Physical RAM: 32768 MiB (8388608 frames)`,
  `Usable Free RAM: 31873 MiB`; write-readback probe passed at 2, 4, 16, and
  30 GiB; boot log preserved at `/mnt/boot.log`.
