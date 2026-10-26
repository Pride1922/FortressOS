# Graphics foundation hardening — 2026-10-26

The experimental Phase 3–4 checkpoint is `03262b3`. This work implements the
approved [hardening plan](../plans/GRAPHICS_FOUNDATION_HARDENING_PLAN.md), without
adding UI features. The implementation branch is `codex/graphics-foundation-hardening`.

## Implemented contracts

- Keep SYS_FBINFO=96; add SYS_FB_ACQUIRE=97, SYS_FB_RELEASE=98, SYS_MMAP=99.
  The ABI high-water mark is 99 and next slot 100. EBUSY is -36, not Linux's
  errno number. The Rust poll/read EINTR comparison is corrected to FortressOS
  -22 (the demo incorrectly used -4, which is ENOSYS).
- Effective-root acquisition grants one exclusive address-space lease. CR3 is
  a stable lifetime identity until final VMM teardown, including all scheduler,
  CPU and operation references. A secondary thread exiting cannot release a
  live process's lease. Thread-group ownership permits release from another
  thread in that same address space. No PID/pointer reuse window exists before
  the final teardown callback clears ownership and the root returns to PMM.
- `fb.c` owns a Rank-1 metadata lock with FREE/TRANSITION/OWNED/BUSY states.
  It never holds that lock across VMM, console, logging, or waits. BUSY excludes
  map/release races and acquire remains blocked until revocation and console
  restoration finish. Failed revocation retains ownership fail-closed.
- SYS_FBINFO validates its normal-RAM output buffer, geometry, physical/page
  alignment, pitch, a 32 MiB cap, canonical limits and stack/restorer exclusion.
  Mapping without the lease is EPERM. Destination collisions are EEXIST and
  preserve the old mapping. Only one device span is recorded; exact repeated
  mapping is idempotent, and a different alias is EBUSY.
- PTE_DEVICE uses software bit 9 to mark borrowed MMIO. Teardown removes device
  leaves and private tables without returning device physical frames to PMM.
  VMM range transactions preflight under the installation lock, prepare bounded
  table spares outside it, and roll back only their newly installed leaves.
  Failure may retain empty private tables until normal address-space teardown;
  it does not retain anonymous data frames. Physical RAM is freed only after
  required synchronous TLB invalidation. No allocations or shootdown waits occur
  under the VMM lock.
- Release validates the entire expected device/physical span, removes it and
  completes full-address-space synchronous TLB invalidation before another owner
  or the console can draw. Old VRAM addresses then fault. Device pages are
  rejected as syscall copy buffers, so a concurrent release cannot invalidate
  a kernel copy into MMIO. User-mode pixel access remains permitted while mapped.
- All console framebuffer paths serialize on the existing Rank-5 console lock.
  Glyphs, terminal cursor, scrolling and clear update only the retained text model
  while graphics owns VRAM. Normal serial/dmesg paths remain active. Release or
  final-space destruction repaints the model without framebuffer reads. Owner
  death prints the requested restoration diagnostic to raw COM1. Panic diagnostics
  retain raw COM1; there is no panic framebuffer override requiring unsafe locks.
- SYS_MMAP(size) grants one zero-filled RW/NX anonymous arena at 0x4000000000,
  rounded safely to pages, with a 32 MiB ceiling. Zero/oversize is EINVAL;
  collision/additional arena is EBUSY. It accepts any user process, serializes
  publication across threads, and rolls back RAM and mappings on failure. There
  is deliberately no individual munmap or POSIX mmap compatibility claim.
- The compositor acquires before allocating/mapping, claims a dynamic 16 MiB
  Talc heap, releases on normal/error returns, and relies on final-space cleanup
  for faults or forced death. ELF admission is restored to 1024 pages / 4 MiB.
- USB per-packet UART diagnostics are off unless compiled with USB_MOUSE_TRACE.
  The USB mouse regression now checks actual compositor pixels and cursor
  restoration instead of depending on those trace strings.

## Verification

- `make test-compositor-host`: 6/6 actual Rust module tests PASS.
- `make test-elf-page-host`: ASan/UBSan page initialization and actual ELF-loader
  rollback tests PASS. A 16 MiB static image is again rejected before allocation.
- `make test-fb-host`: actual VMM, display state machine and console under
  ASan/UBSan PASS. Device failures span the first/middle/final table allocation
  cuts across a PT boundary; anonymous failures include data and table OOM.
  Every cut checks exact final frame/heap reclamation. The PMM mock rejects any
  attempt to free borrowed 4 GiB MMIO. Tests cover collision preservation, wrong
  physical-span revocation, invalid/reserved ranges, idempotence, alias refusal,
  failed release, an eight-way owner race, and a paused revocation proving the
  next owner cannot acquire before unmap/invalidation completes. Console tests
  cover clear, terminal escapes, text cursor and scroll without owned VRAM writes.
  Existing anonymous collisions also return immediately with zero PMM allocation
  budget, before any data/table allocation; preflight is repeated after preparation
  to close concurrent-map races.
- `make test-abi-version-host`: all 100 syscall slots and existing ABI checks PASS.
- `make test-fb-ownership`: BIOS/UEFI x SMP=1/4, 4/4 PASS. Real compositor process
  readiness is synchronized through a pipe. A second compositor exits busy, owner
  SIGKILL is reclaimed, and a later compositor starts. Text-shell recovery passes.
- `make test-fb-teardown`: BIOS/UEFI x SMP=1/4, 4/4 PASS. Actual user processes test
  non-owner map/release, invalid output, collision, no extra aliases, anonymous
  rounding/zeroing, concurrent console writes without a single changed VRAM pixel,
  shared-thread release/remap and thread exit, normal owner exit, #UD, stale
  post-release #PF, forced SIGKILL of a blocked owner, and eight simultaneously
  released process contenders with exactly one winner. Twenty repeated map/exit
  cycles after warmup preserve exact free-frame/table/deferred counts, including
  anonymous allocations in each child. Additional user probes confirm anonymous
  execute attempts fault (#PF/NX), exactly one shared-thread arena request wins,
  and VRAM cannot be passed as a syscall copy buffer.
- Real PMM `rejected_frees` is read before/after both QEMU campaigns through QMP's
  virtual-memory monitor while all vCPUs are stopped. Symbols come from the exact
  kernel frozen into the private ISO; no extra diagnostic syscall or observer is
  compiled into release media. All eight boots record 0 -> 0. JSON evidence also
  records the frozen kernel and private initramfs SHA-256.
- `make test-compositor-qemu`: existing font/window/cursor pixel acceptance PASS
  on BIOS/UEFI at 1280x800 with the dynamic heap and display lease.
- `make test-usb-mouse`: BIOS direct-root / UEFI hub PASS, including actual
  compositor cursor motion, restoration, right-click exit and USB keyboard help.
- GCC stack gate (-Wframe-larger-than=1024) passes for VMM/display objects.
  `map_user_span` is 368 bytes; display functions peak at 128 bytes. Rust remains
  freestanding without SIMD. No SIMD state/boot/IRQ/scheduler protocol changes.

Evidence files: `build/graphics-hardening-final-host.log`,
`build/graphics-hardening-final-qemu.log`,
`build/graphics-hardening-counter-qemu.log`, `build/fb-stack-gate.log`,
`build/graphics-hardening-nx-qemu.log`,
`build/fb-admission-host.log`, `build/fb-admission-qemu.log`,
`build/fb-{clients,check}-{bios,uefi}-{1,4}.log`, and
`build/fb-counters-{clients,check}-{bios,uefi}-{1,4}.json`.
Lifecycle probes are injected only into temporary ISO/initramfs copies and never
added to production release media. Tests attach no data disks and reap QEMU.

## Retained findings and limits

An early ownership test incorrectly raced its own acquire attempts against
compositor startup and could take the lease first. It was replaced with a pipe
readiness handshake. Host revocation testing caught applying a leaf-only device
flag to intermediate-level walk requirements; the walk now checks that bit only
on the leaf. Console testing caught an ungated clear-screen path; it is gated.

This is a bounded graphics foundation, not a general VM API or isolated window
server. Windows remain in one compositor process; resizing, external-client IPC,
input-seat ownership, hotplug and non-default framebuffer color layouts are not
accepted here. A dynamic 16 MiB heap still bounds supported backbuffer sizes.
There is no vblank/page-flip ABI, physical-hardware acceptance or performance/idle
CPU claim. Global process termination semantics for live sibling threads are an
existing separate scheduler concern: display ownership is retained until actual
address-space quiescence, never released on an assumed "last" thread alone.
An exit/kill of a leader with surviving sibling threads is not accepted as a
process-wide termination guarantee in this batch; that scheduler audit remains
explicit follow-up work. The display lease remains held while that VM is live.

Unrelated networking/Wi-Fi edits appeared in the shared working tree during the
campaign. They are left untouched and excluded from the graphics commit; these
results describe the frozen kernels actually booted, not an independent acceptance
claim for those concurrent changes.
