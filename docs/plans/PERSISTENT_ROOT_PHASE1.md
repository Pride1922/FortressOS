# Persistent root Phase 1 implementation contracts

Phase 1 supplies executable and namespace mechanisms. Production PARTUUID
admission, mutable accounts, authenticated recovery, installed homes and shutdown
routing remain later phases. Normal make and live ISO/image behavior are preserved;
installer media remains a separate Phase 4 artifact family. No physical disk I/O
is authorized by this checkpoint.

## Executable ownership

`vfs_exec_image` returns an owned heap image bounded by the shared 4 MiB limit.
Initial login and subsequent VFS spawn consume that image through the existing ELF,
credential, descriptor, process-record and scheduler publication code, then free
it and close the file on every outcome. Initial login uses explicit root actor
credentials from a BSP kernel supervisor with preemption disabled until terminal
handoff. It neither requires a current user process nor borrows TarFS `data`.

EXT4 captures authoritative executable metadata, permission admission and complete
bytes under one ranked filesystem exclusion. The loader runs after exclusion is
released. Trusted detached roots allow reviewed set-ID semantics; external `/mnt`
mounts remain nosuid/nodev. A frozen mount reports readonly consistently through
authoritative metadata and permission adapters. Existing external nosuid adapters without a snapshot retain their bounded read
fallback; unknown trusted mutable adapters reject execution. Callback copies are never shared
with the loader or retained after its consumption.

## Namespace lifecycle

`ext4_prepare_root` reserves the existing single mount slot and prepares journal
recovery, activation, orphan cleanup and a detached root. Its caller must admit
exclusive recovery/write policy before entry. It introduces no device discovery
or physical-write admission. Preparation blocks process publication. Failure or
explicit discard clears the block and leaves bootstrap lookup active. Discard
requires all validation references and opens released; it cannot undo recovery
writes. `ext4_sync` and freeze also accept this owned detached mount.

`ext4_publish_root` makes one boot-only namespace change and retains a boot-lifetime
root reference. It reuses the exact existing devfs/runfs nodes for `/dev`, `/run`
and `/tmp`; persistent placeholder contents are hidden. VFS routes traversal
before filesystem lookup, preserves search admission and owned references, and
makes `..` at namespace roots stay inside the selected namespace. Mount-point
unlink/rename is rejected. Root paths have a single leading slash. Cwd remains a
canonical string; publication precedes installed-process creation.

The bootstrap tree, file handles and Limine module backing survive publication.
Explicit `vfs_lookup_at_creds` and `vfs_bootstrap_lookup_creds` select a borrowed
namespace root and return owned nodes. There is no user root-switch syscall or
runtime root reversal. The first process publication seals root attachment,
including staged processes. Failed construction does not publish a process.

## Verification fixture

`make test-root-phase1-host` creates fresh regular images and runs actual VFS,
EXT4 and JBD2 with sanitizer host adapters at 512/4096 sector sizes, plus the
actual ELF loader and set-ID/secure-entry suites.

`make test-root-phase1` adds an isolated `ROOT_PHASE1_TEST=1` kernel, an explicit
fw_cfg gate, QEMU NVMe-only disposable copies, BIOS/UEFI and SMP=1/4/8. No ordinary
live entry selects this root. Legacy Ring 3 diagnostics are skipped in this
fixture so they cannot precede namespace publication; required BSP syscall-MSR
initialization remains in place. AP initialization and scheduler startup remain
in place; unrelated SMP worker cohorts and the legacy TCG-sensitive TSC timing
diagnostic are skipped in this dedicated fixture. The fixture validates, freezes and publishes
its root before login. It retains existing readonly account policy and `/run`
homes rather than claiming later installed-account support.

Guest-only snapshot fault adapters exercise OOM and short-read unwind paths;
they are not hardware error injection. Existing loader fault adapters and
100 supervisor spawn/exit cycles check process resources. Cache churn and fully
pinned saturation exercise the 1024-node bound. Independent Linux fsck audits
follow actual host and guest runs. Exact argv, source hashes, isolated build
manifest, transcripts and failed runs remain under `build/root-phase1`.

Executed evidence is recorded in
[Phase 1 evidence](../roadmap/persistent-root-phase1.md).
