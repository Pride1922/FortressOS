# SMP Piece 1 — AP Discovery

Status: implemented, **not yet verified**. Nothing in this file is evidence
until someone actually runs the steps below and records what happened.
Per [`SMP_DESIGN.md`](../../SMP_DESIGN.md)'s workflow, Piece 2 does not
start until this piece's verification is reported back as passing.

## What this piece does

Scope is deliberately narrow: find every CPU, start it, prove it's alive.
Nothing else.

- `smp_init()` ([src/arch/x86_64/smp.c](../../src/arch/x86_64/smp.c)) reads
  Limine's SMP response and cross-checks every reported LAPIC ID against
  ACPI's MADT enabled-CPU list (`SM1` — MADT is the source of truth, not
  Limine's count on its own).
- Every non-BSP CPU is handed off via Limine's `goto_address` protocol
  (`SM2`) to `smp_ap_entry`, which does exactly two things: atomically
  mark itself online, then `cli; hlt` forever.
- The BSP waits (bounded spin, no real timer available this early) for
  every AP to report in, then logs a pass/fail summary.
- Called from `kmain()` as a new checkpoint, `SMP Piece 1: AP Discovery`,
  placed after the Phase 9C.2 ext2 suite and before shell startup — see
  [src/kernel/main.c](../../src/kernel/main.c) (`test_smp_piece1_ap_discovery`).

### Deliberate deviation from the original SM2 text

`SMP_DESIGN.md`'s first draft of `SM2` assumed a hand-rolled INIT-SIPI-SIPI
trampoline in identity-mapped sub-1MiB memory. Limine — already
load-bearing for this kernel's boot — performs that exact sequence itself
as part of its documented SMP protocol, before `kmain()` is ever reached,
and parks every AP spinning on its own `goto_address`. Reimplementing
INIT-SIPI-SIPI by hand would duplicate something Limine already has to
get right for its own boot to work, for no benefit, so this piece reuses
it instead. Flagging this here since it wasn't what the design doc
originally described.

### What Piece 1 deliberately does NOT do

No per-CPU GDT/TSS, no IDT on APs, no scheduler involvement, no shared
kernel state touched by an AP beyond one atomic counter. An AP that
reports in just halts. That's Piece 2 (per-CPU storage) and later.

## How to verify

### QEMU (automated)

```bash
make test-smp-discovery
```

This boots the kernel twice under QEMU/TCG (software emulation, so this
proves logical correctness, not real hardware AP timing):

1. `-smp 1` — confirms the single-CPU path is unchanged: MADT/Limine
   agree on one CPU, no APs to start, boot still reaches the shell.
2. `-smp 4` — confirms 3 APs are discovered, matched against MADT, start,
   and report in, and boot still reaches the shell.

Both runs assert no `[FAIL]` appears between the `SMP Piece 1: AP
Discovery` and `SMP Piece 1 (AP discovery) complete.` markers in the
serial log. Look at `scripts/test_smp_discovery.py` if either assertion
fails — it prints the relevant serial log tail.

If you want to sanity-check by hand instead of via the Makefile target:

```bash
make bin/fortress.iso build/nvme_gpt.img
qemu-system-x86_64 -M q35 -m 2G -smp 4 -serial stdio \
  -drive file=build/nvme_gpt.img,if=none,id=nvm0,format=raw \
  -device nvme,serial=fortress0,drive=nvm0 \
  -boot d -cdrom bin/fortress.iso
```

Look for, in order: `Limine SMP response cross-checked against ACPI
MADT: 4 CPU(s) agree`, then `All 3 application processor(s) online
(parked, interrupts disabled)`, then `SMP Piece 1 (AP discovery)
complete.`, with normal boot continuing to the shell prompt afterward.

### Dell Latitude 5590 (physical hardware)

The Dell 5590 (Core i5-8350U) is 1 socket / 4 cores / 8 threads. Real
hardware is what actually exercises real INIT-SIPI-SIPI timing on real
silicon — QEMU/TCG does not.

1. Build and boot from USB exactly as for prior phases (see
   [subsystems.md](subsystems.md), "Dell Latitude 5590 physical
   acceptance", for the general boot procedure).
2. Watch the serial/COM1 log (or the framebuffer console if COM1 is
   absent) for the same three markers as the QEMU case above. Expect
   `4 CPU(s) agree` and `All 3 application processor(s) online` on this
   machine specifically (a different physical machine will have a
   different core count — check its own MADT-reported count instead).
3. Confirm the rest of boot is unaffected: shell still comes up, `ls`,
   `cat`, existing storage/USB checks still behave exactly as before this
   piece landed. This piece must be a no-op for everything except the new
   checkpoint.
4. If you have a way to keep the machine running for a while afterward
   (e.g. leave it at the shell prompt), watch for any instability — this
   is the first time this kernel has ever let another core execute code,
   and an AP miscounting itself or double-reporting would be a subtle
   thing to catch only under real timing, not something the spin-timeout
   bound in `smp.c` was tuned against yet.

### What "pass" looks like

- Both QEMU runs (`-smp 1`, `-smp 4`) pass `make test-smp-discovery`.
- Dell hardware shows the same three markers with counts matching its own
  MADT (4 CPUs on the 5590), and normal boot/shell/storage behavior is
  unchanged.
- No `[FAIL]` or `[WARN]` lines from the SMP Piece 1 checkpoint on either
  QEMU or hardware. A `[WARN]` (online AP count disagreeing with MADT
  after the timeout) is not a hard boot failure by design — it degrades
  to single/partial-CPU rather than halting — but it means the spin
  timeout in `smp.c` needs recalibrating before this piece counts as
  verified, not that it can be waved through.

### If it fails

Report back: which run failed (QEMU `-smp N`, or Dell), the exact
`[FAIL]`/`[WARN]` line and surrounding serial output, and whether normal
boot (shell, storage) still worked despite the SMP failure. That decides
whether it's a `smp.c` bug or just the spin-timeout constant needing
tuning for real hardware.

## Evidence

*(Empty until an actual run happens — do not fill this in from the design
alone.)*

| Date | Environment | `-smp` / core count | Result | Notes |
| --- | --- | --- | --- | --- |
| | | | | |
