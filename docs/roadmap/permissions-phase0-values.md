# Permissions Phase 0: values and registry audit checkpoint

2026-10-10. Phase 0 foundation COMPLETE locally; no enforcement.
No GitHub issue/project state is changed by this checklist.

## Local delivery checklist

- [x] Registry field/operation ownership map and caller lifetime source audit.
- [x] Isolated bounded credential values, validation and transition helpers.
- [x] Actual-code credential host sanitizer tests and freestanding compilation.
- [x] Deterministic host reproduction of merge/exit and stale-slot corruption.
- [x] #5 prerequisite: user approved global registry exclusion; implemented.
- [x] Registry correction: positive host regressions and disposable SMP QEMU checks.
- [x] #5 local foundation gate: implement and verify credential binding/publication.
- [x] #5 local design gate: document authorization/mutation protocol and current filesystem ownership.
- [ ] Phase 1/2: implement and verify actor-aware authorization adapters; no enforcement approval implied.
- [x] Phase 0: credential TCB binding, spawn snapshots and complete publication.
- [x] Phase 0: metadata, compatible stat ABI (#7), filesystem mode retention.
- [x] Phase 0: minimal devfs/runfs and focused host/Ring 3 acceptance.
- [x] Approved EXT2 owned-reference retirement and focused lifetime regression.
- [x] Phase 0: complete existing EXT4 integration/crash regression gates.
- [x] Approved bounded GPT boot scratch repair and BIOS/UEFI storage checks.
- [x] Phase 0: final raw USB devfs/filesystem peer acceptance, BIOS/UEFI × SMP=1/4.
- [x] Phase 1: permissive wiring and finite bypass audit; see [Phase 1 evidence](permissions-phase1-wiring.md). Authoritative enforcement adapters remain open.
- [ ] Phase 2: enforcement, metadata syscalls/tools and denied-mutation gates (#6).
- [ ] Phase 3: database, login, credential syscalls and whoami.
- [ ] Phase 4: set-ID, sudo, secure spawn and nosuid.
- [ ] Phase 5: hardening, fuzzing and targeted physical acceptance.

Overall Phase 0 is checked complete for the documented foundation and finite gates.
Phases 1–5 remain unchecked. The user authorized completion of Phase 0 and
explicitly approved registry consolidation, EXT2 lifetime and GPT scratch repairs. The current
foundation is not permission enforcement. Shared user threads require a shared
credential authority first; cross-core sockets and memory performance remain
separate. The evidence below starts with the historical audit checkpoint.

## Executed evidence

Working tree at `C:\Sources\FortressOS`, WSL Ubuntu-24.04:

1. `wsl -d Ubuntu-24.04 -- make test-perm-creds-host`: PASS, ASan/UBSan,
   70,496 matrix cases plus focused checks. Actual `src/kernel/creds.c`;
   no process, IRQ or scheduler adapters are used by the value helpers.
2. `wsl -d Ubuntu-24.04 -- make build/kernel/creds.o`: PASS with kernel flags.
3. `wsl -d Ubuntu-24.04 -- make audit-process-registry-host`: reproduced
   finalized ticks 30 -> 20, and PID 3's sample overwriting PID 4's ticks to 20.
   Exit status 0 means the expected defect was observed, **not** correctness.
4. `wsl -d Ubuntu-24.04 -- make build/kernel/process_table.o`: PASS with the
   host audit barrier excluded.
5. `git diff --check`: PASS.

The audit harness links the actual registry with pthread mutex adapters and
ASan/UBSan. Its condition-variable barrier pauses after the merge predicate,
orders exit/reuse before the delayed store, then reads the result after join.
This is a controlled admitted-operation interleaving, not a scheduler/IRQ or
physical acceptance claim. The host-only hook is guarded by both
TEST_PROCESS_REGISTRY_AUDIT and TEST_SMP_MEMORY, and the runner times out in
15 seconds. No disks or guest images are involved. No QEMU/Dell runs occurred.

The initial O1 audit compilation failed under `-Werror=maybe-uninitialized`:
`process_signal_send` can jump to `out` before initializing `woken_count`
when the caller is missing/exited. The audit uses O0 for the controlled
interleaving and does not invoke that branch; the warning is recorded rather
than suppressed or silently repaired. A focused missing-caller/exited-caller
test should be part of the separately accepted registry correction.

## Reviewed correction (approved and implemented)

Use the existing ordinary rank-1 G lock for every registry lookup, lifecycle
transition, name/tick update and signal action/frame operation. Remove shard
use from this module. Remove self/singleton shortcuts that look up records
outside G; single/group sends select and publish under G, capture bounded PIDs
and wake after unlock. Initialize wake count before any error jump. Keep
attached staged-child signal eligibility unchanged. Detach signal bindings
explicitly on abort; no pointer returns to callers.

Keep `take_action` stop-state/event publication in the same G critical section,
eliminating its S/G handoff. Retain atomic mailbox publication/acquire scheduler
reads and lock-free owned group reference cloning. No scheduler/VFS/network
locks nest with G. Preserve all scheduler placement, VMM lifetime, atomic walks,
deferred reclamation and context handoff behavior. Do not remove or broaden
the generic PROCESS-kind lock-checker exception as part of this correction;
it has a separate documented discrepancy and needs its own scope decision.

Acceptance before credential binding: convert the reproduction into positive
ordering regressions (exit/reuse must wait for admitted merge), missing/exited
caller error tests, concurrent snapshot/refresh/exit/reuse, action/CHLD and
group membership coverage, existing process/group/stops/orphan host suites,
then existing disposable BIOS/UEFI SMP metadata/signal acceptance with exact
cleanup. No throughput improvement is promised; profiling remains paused.

The user explicitly approved global-lock consolidation on 2026-10-10.
The correction above is implemented in process_table.c and its public header.
The generic PROCESS-kind checker exception is untouched. No TCB credential
binding, syscall or VFS authorization implementation was added.

Post-correction host evidence: `make test-process-registry-host` PASS at O1
with ASan/UBSan: same-lock/blocked-exit checks, final ticks 30 retained, reuse
and stale-PID rejection, missing/exited caller errors, self/group/staged
signals, wake-after-unlock, and concurrent CHLD action/inheritance with 1000
child lifecycles. The former audit target is now this positive regression.
The earlier O1 uninitialized-wake-count warning is fixed and its error branches
are exercised.

Existing host suites PASS: `make test-s8-process-host test-s9-metadata-host
test-s8-signals-host test-s8-stops-host test-s8-orphans-host`, plus
`python3 scripts/test_process_table_host.py --groups`. These remain pthread
adapters, not IRQ/scheduler evidence.

`make iso build/s8_signal_user.elf` PASS; `bin/fortress.img` is untouched.
`env SMP=4 python3 scripts/test_s8_process.py --signals` PASS BIOS/UEFI,
disposable test ISO/OVMF and no data disks; logs are
`build/s8-signal-bios-4.log` and `build/s8-signal-uefi-4.log`.

The first metadata runner attempt reached guest PASS and the current shell
prompt but waited for obsolete `fortress> `. Interrupted that attempt, retained
`build/permissions-registry-metadata-old-prompt.log`, and explicitly terminated
its identified leftover QEMU PID. Updated only the runner's prompt matcher to
accept the current `fortress:/ $` form; the full matrix is rerun below.

`python3 scripts/test_s9_metadata.py` PASS, 6/6 BIOS/UEFI x SMP=1/4/8:
all owner CPUs, concurrent reader, final ticks, zombie/reap/wait and prompt
recovery. Exact-argv preflight rejects extra drives/blockdev/devices/hda/snapshot.
Each guest uses a disposable ISO and OVMF variables, with no data disks.
Evidence: `build/s9-metadata-{bios,uefi}-{1,4,8}.log` and matching `.stderr`.
Post-run `pgrep -af qemu-system` found no remaining test QEMU processes.
`git diff --check` PASS. These gates establish this registry correction's
focused acceptance, not credential/VFS permission enforcement or performance.

## Phase 0 implementation evidence (current foundation)

- Credential registry sanitizer regression: 20,000 alternating complete
  publications/snapshots, invalid/stale rollback, staged/exit/abort/reuse gates,
  ordinary root cap-drop inheritance; existing process/group/signal suites pass.
- Gated BIOS/UEFI SMP=1/4/8 metadata fixture: 6/6 pass, including real AP
  credential snapshots while owners publish then exit. `build/s9-metadata-*`
  retains firmware/CPU logs. This tests actual scheduler execution within the
  finite fixture, not every IRQ/lifetime interleaving.
- `make test-perm-fs-host`: ASan/UBSan pass for actual VFS/TarFS/devfs/runfs,
  complete mode/owner metadata, malformed USTAR rejection, bounded capacity,
  owned unlink lifetime and concurrent shared-offset append.
- `python3 scripts/test_perm_inodes.py`: pass,
  `build/permissions-phase0/inodes-zlzmuapa`. EXT2 128/256-byte and journaled
  EXT4 inodes roundtrip all 4096 permission modes with UID 0x12345678 and GID
  0x87654321; independent Linux fsck/stat passes. EXT2 additionally verifies
  held-reference retirement/reuse, active-open unlink/replacement with zero
  writes/flushes, open-source rename and descendant paths. The earlier ASan
  reproduction remains in `build/permissions-phase0/ext2-lifetime-repro.log`.
- Disposable Ring 3 Phase 0 probe: BIOS/UEFI × SMP=1/4, 4/4 without data disk
  (`guest-ejxaxzdr`) and 4/4 with disposable journaled EXT4 (`guest-agav8cl9`),
  under `build/permissions-phase0`. Exact legacy/extended stat buffer canaries,
  size/version/error handling, TarFS/devfs/runfs/EXT4 metadata and ordinary
  child spawn after gated capability drop pass. No credential syscall added.
- `make test-host bin/fortress.elf bin/initramfs.tar bin/fortress.iso`: pass
  after EXT2 lifetime repair, including eight existing EXT2 geometry profiles.
- Existing shell BIOS/UEFI passes; the no-UART test initially failed its obsolete
  prompt matcher, then passes after recognizing the current shell prompt.
  `python3 scripts/test_shell_no_uart.py`: UEFI 8 GiB, PS/2 echo/sleep PASS.
- Existing `test-ext4-write-host` six geometry profiles, independent Linux byte
  and fsck audits pass (`build/ext4-phase4/host-1m7fsc17`). Allocation/max-map
  regression passes after reserve-template mode correction
  (`build/ext4-phase3/run-2gdwbruj`).

All host lock adapters are explicitly mocks; no host result proves IRQ/scheduler
correctness. No Dell tests, GitHub mutations, allocator/TLB changes or normal
`bin/fortress.img` rebuild occurred. New EXT2 fixtures explicitly select ext2;
ordinary image policy remains journaled EXT4. Mutable actor wiring, DAC/sticky
checks, denied-mutation tests (#6), `_kernel` entry points and set-ID clearing
are later gates, not established by the metadata foundation.

Additional focused results: EXT4 create-attributes recovery passes 1000 cuts
across four sector-atomic persistence models; recovered metadata is absent or
complete (mode 06751, full-width UID/GID). Both representative recovered images
pass independent Linux fsck/stat; `build/permissions-phase0/attrs-recovery-*`.
The reproducible inode target includes this gate. Final EXT2 lookup I/O/OOM
rollback and 128-byte mode matrix pass (`ext2-final-128.img`), with clean fsck.
`make test-ext2-write` passes BIOS/UEFI three-boot persistence, truncation,
namespace cleanup and six Linux clean-state audits on explicit disposable EXT2
copies. Current mounted Phase 0 Ring 3 rerun passes 4/4 in `guest-r_k232ad`.

EXT4 existing mounted host regression completes 12/12:
`build/permissions-phase0/mount/host-qdisxibr`, covering owned open/unlink,
recovery admission, failed publication, freeze/taint and pthread exclusion.
Namespace regression completes 12/12 with 324 Linux oracle copies:
`build/ext4-phase8-3/run-0tjkb4gv`. New creation defaults intentionally change
regular/directory mode to 0644/0755; the namespace Linux oracle expectation for
new directories was updated accordingly, with all other checks retained.

EXT4 QEMU integration: SMP=1 completes 6/6 in
`.codex-remote-attachments/permissions-phase0/integration/guest-smp1-umqk2f8d`.
SMP=4 first attempt passed five cases but UEFI/4096 boot failed the existing
TSC-offset self-test before filesystem setup (estimated 102 us under concurrent
TCG load). The isolated selected-case rerun passes 1/1, both boots, in
`guest-smp4-ohxmo7zg`; combined coverage is 6/6. The failed attempt `guest-smp4-wevjz0ep` is retained,
and no timing threshold, scheduler or performance code was changed. The runner
now accepts an explicit firmware/block-size selector and accurately reports the
selected count. Fixture copies retain source hashes and provenance independently
of test acceptance in `.codex-remote-attachments/permissions-phase0/fixtures`.

The existing Phase 9.4 guest crash campaign completes 66/66: transaction 36
(`guest-crash-ttfc6sqd`), recovery 12 (`guest-crash-h6i9s7vn`), AP append 12
(`guest-append-crash-egx6os2n`) and open-unlink orphan 6
(`guest-orphan-crash-rs2ifed9`), under `.codex-remote-attachments/ext4-phase9`.
`phase9-4-review.json` independently reports 66 cases and zero errors. The
isolated workspace `guest-workspace-1c92wrgm` predates the final raw-read adapter;
the adapter does not alter the covered filesystem mutation/recovery paths.

Devfs raw USB reads now join the boot-selected EXT4/EXT2 filesystem exclusion
per sector, preserving the shared synchronous BOT transport contract. Actual
filesystem host ASan/UBSan checks pass for both exclusions, sector-straddling
bytes and I/O failure without fallback/retry (`test-perm-device-host`). The
routing-only devfs fixture also passes; it is not an IRQ/scheduler proof.

Initial final raw USB guest acceptance failed. `guest-d0_rqzzn/bios-1.log`
reproduces a double fault before mounting or executing the permissions probe.
GDB frame walk in `guest-41gny486/panic-gdb.log` establishes the live chain:
`kmain -> xhci_boot_probe -> xhci_init_one_controller -> gpt_parse ->
gpt_parse_ex -> gpt_read_and_verify_array -> kmalloc -> kmalloc_unlocked ->
heap_expand`. Its stack probe/store crosses the existing 16 KiB boot guard.
Relevant sources: `src/fs/gpt.c:554` (16-entry local staging array),
`src/fs/gpt.c:365` (partition-array allocation), `src/mm/heap.c:160`
(expansion frame) and `src/arch/x86_64/boot.asm:34` (stack bound).
This allocation-dependent stack exhaustion is an additional discovered defect,
not evidence that host lock adapters certify the USB path. No stack size,
allocator, VMM or xHCI workaround was changed. The user approved moving the
16-entry staging array to bounded static scratch. GPT already uses global
registry/sector scratch; its boot-only non-reentrant contract is now explicit
in the header. Reset-on-entry and all-or-nothing publication remain unchanged.
`make test-storage` passes BIOS/UEFI GPT, EXT2 Ring 3 and allocation-set audits.
The final raw USB probe passes 4/4 BIOS/UEFI × SMP=1/4 in `guest-zejmwym_`:
read-only `/dev/sdap1` metadata/write rejection, repeated raw reads alongside
mounted-file writes, exact bytes/length, cleanup and dropped-cap child spawn.
SMP=4 peer I/O does not prove the Ring 3 child executed on an AP. The failed
attempt and GDB evidence remain retained. This closes the final local gate.

The normal journaled EXT4 `bin/fortress.img` remains unchanged; read-only policy
and SHA-256 evidence is `build/permissions-phase0/normal-image-preserved.json`.
