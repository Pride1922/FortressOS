# EXT4 Phase 9.4 — Disposable guest crash execution

Declared finite gate passed 2026-10-06: 66/66 native guest cases, six normal
SMP=4 regression cases (12 boots), and evidence review. See
[results and limits](../roadmap/ext4-phase9-4.md).

Authorized after verified Phase-9.3 host acceptance. Production mount gates,
USB dispatch, protected synchronization and DMA contracts remain unchanged.

Use an isolated source snapshot and build directory, then immutable ISO and
ELF snapshots with matching hashes. Never boot a changing shared build output.
Each case owns its GPT/NVMe regular-file fixture and UEFI variables; exact QEMU
argv rejects additional disks, host devices and implicit snapshot semantics.

Start with BIOS, 1 KiB blocks and one CPU. Use audited GDB hardware breakpoints
in the actual transaction/recovery path, then terminate the stopped guest
without sync or freeze. Record the symbol, hit condition, writer state, sequence,
metadata/data counts and debugger transcript. A timeout or missed milestone is
a failed case, never a crash pass.

Transaction milestones cover ordered-data submission, durable commit before
checkpoint, checkpoint in progress and an admitted recovery interruption.
Use the existing explicit journal fixture admission. Independent Linux replay
on a copy derives the expected byte/namespace result before the FortressOS
restart; retain pre-crash and post-crash hashes and raw Linux diagnostics.
Recover with a fresh guest, freeze normally, and require exact independent
bytes/namespace/ownership and unmounted fsck. Retain every failed input.

After the first vertical case passes, expand BIOS/UEFI, 1/2/4 KiB and SMP=1/4.
Include AP shared and independent append and descriptor/orphan lifetimes.
Declare and verify exact finite milestone coverage before acceptance.

QEMU termination does not discard the host page cache. Document the selected
block backend cache mode and keep guest crash evidence separate from the host
volatile-cache, reorder and tear model. No physical power-loss claim is made.
Use bounded startup/debugger/recovery waits, reap every owned process and keep
evidence outside build. Never attach physical storage or invoke automatic fsck
repair to conceal an unexplained corrupted input.
