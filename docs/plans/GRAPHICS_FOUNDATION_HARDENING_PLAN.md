# Graphics Foundation Hardening

Status: core ownership/mapping/allocation batch implemented, 2026-10-26;
verification and remaining process-termination limits are recorded in
[the hardening evidence](../roadmap/graphics-foundation-hardening.md).
The Phase 3–4 compositor demonstrates rendering and interaction; it is not a
stable display ownership API. No further UI features are included in this plan.

## Delivery order

1. Checkpoint the functional demo, prerequisites, tests and evidence with:
   `feat(ui): initial Rust compositor with windows, cursor, and font rendering`.
   Body: "This is a functional demo. It contains known framebuffer ownership and
   display collision bugs documented in the graphics hardening plan."
2. Harden framebuffer mapping, display ownership and process cleanup. Validate
   this stage independently before changing allocation policy.
3. Replace the static heap with bounded anonymous process memory, restore the
   ELF image ceiling to 4 MiB, and validate allocation failure/reclamation.
   Final hardening commit: `fix(kernel): harden framebuffer ownership, enforce display locks, and clean up compositor allocation`.

Checkpoint completed as `03262b3`. Implementation uses an address-space lifetime
identity (CR3 held until its final VMM teardown), not a recyclable thread PID.
SYS_MMAP deliberately provides one fixed process-private arena at 0x4000000000,
up to 32 MiB; additional requests return EBUSY. The committed PTE span is its
bookkeeping and concurrent publication is serialized by the VMM lock. This is
not POSIX mmap and has no munmap/growth API. Display acquisition requires
effective UID 0, following the existing dmesg policy. EBUSY is FortressOS -36.

## ABI and mapping invariants

- Keep SYS_FBINFO=96. Allocate SYS_FB_ACQUIRE=97, SYS_FB_RELEASE=98; reserve
  SYS_MMAP=99 for anonymous memory. Recheck slots before implementing, and update
  maximum/next syscall constants, ABI documentation, wrappers and ABI tests.
- Acquire is exclusive and process/address-space scoped, including shared-VM
  threads. Use an identity whose lifetime cannot collide with PID/pointer reuse.
  Repeated acquire is EBUSY; non-owner release/map is EPERM. No implicit acquire.
- Only the owner may map VRAM. A process has at most one recorded framebuffer
  mapping. Repeated identical mapping may return the existing metadata; any
  different mapping request while one exists must fail without adding aliases.
- A device PTE software bit or explicit VMM mapping record means borrowed device
  memory, never PMM-owned RAM. Do not infer ownership from PCD/cache attributes.
  Teardown skips device frames but frees their private page tables. Audit unmap,
  rollback, validation and all other paths interpreting PTE ownership.
- Check geometry, pitch, physical alignment, mapping size, canonical boundaries,
  reserved regions and overflow before mutation. Reject destination collisions;
  never silently reuse unrelated mappings. Serialize concurrent mutations.
- Mapping failure removes only leaves created by that operation, leaves existing
  mappings/data untouched, and completes required invalidation. Existing VMM
  page-table retention is accounted separately from leaked data frames.
- Release revokes every recorded framebuffer alias and completes synchronous TLB
  invalidation before publishing the display as available to the next owner.
  Failed revocation retains ownership fail-closed. Stale pointers must fault.

## Display ownership and lifecycle

- Serialize acquire/release with framebuffer console presentation, including
  putc/puts, terminal parser, clear, scroll and text cursor. A global bool without
  synchronization is insufficient. State transitions must respect lock ranks;
  never hold console Rank 5 while entering VMM Rank 3 or scheduler/process locks.
- Preserve COM1 and dmesg while ordinary console framebuffer output is suppressed.
  Restore a readable console from its retained text model when ownership ends.
- Owner death must release ownership only after its mappings cannot be used by
  any live thread/CPU. Handle exit, signals, faults and last-thread teardown;
  one pthread exiting must not revoke another live thread's display.
- Emergency panics retain raw COM1 diagnostics; explicitly document any emergency
  framebuffer override rather than silently hiding critical diagnostics.
- Compositor acquires before mapping, reports a busy display cleanly, and releases
  on every normal/error exit after acquisition. Kernel cleanup handles crashes.
- Per-packet USB mouse UART traces are disabled by default, optionally enabled by
  an explicit diagnostic build. Lifecycle/error logging remains available.

## Anonymous memory stage

- Minimal SYS_MMAP(size) maps bounded, zero-filled process-owned RW/NX anonymous
  RAM at a kernel-selected process-private address. Reject zero/overflow/over-cap
  sizes; round safely to pages. Region allocation must serialize across threads.
- Use checked region bookkeeping, no overlap with ELF/stack/restorer/device
  ranges, rollback on failure, and complete reclamation when the process dies.
  Individual munmap is not required for the startup heap in this stage.
- Map a dynamic compositor heap and claim it through Talc. Restore MAX_ELF_PAGES
  to 1024 (4 MiB image). A 4 MiB BSS arena does not fit that image ceiling once
  code/data are included. If allocation is deferred, explicitly choose a smaller
  arena and supported lower framebuffer mode; never leave a silent 20 MiB limit.

## Required verification

- `make test-compositor-host`, `make test-elf-page-host` and ABI tests pass.
- `make test-compositor-qemu` (alias of existing acceptance) passes BIOS/UEFI.
- `make test-fb-ownership`: two contenders, EBUSY while owned, later successful
  reacquire; non-owner release/map rejection; same-process thread behavior;
  concurrent SMP acquire; explicit release and former-owner stale-pointer fault.
- `make test-fb-teardown`: normal exit, forced termination and faults, repeated
  cycles with quiescent reaping. Compare pre-launch/post-reap PMM data/table and
  rejected-free counters, not an unchanged count while the process is running.
  A rejected attempt to free VRAM is a failure even if free count is unchanged.
- Actual-code host failure injection at first/middle/last mapping operations,
  collisions, invalid geometry, repeated calls, reserved ranges and OOM. Verify
  no foreign leaf changes; distinguish retained table frames from data leaks.
- Verify concurrent console output appears in COM1/dmesg without changing owned
  framebuffer pixels, and text console works after release/crash.
- Anonymous-memory validation: zeroing, RW/NX, rounding/bounds, collision/OOM
  rollback, shared-thread allocation serialization and exit reclamation.
- Capture exact commands/results and qualifications in roadmap evidence. Keep
  QEMU media disposable/ISO-only; no physical acceptance or vblank tear-free
  claim. No tests may change boot/IRQ/storage isolation contracts incidentally.
