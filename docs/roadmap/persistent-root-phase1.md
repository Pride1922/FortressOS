# Persistent root Phase 1 evidence

2026-10-10. **COMPLETE — Phase 1 plumbing and disposable acceptance gates.**
Contracts: [Phase 1](../plans/PERSISTENT_ROOT_PHASE1.md).

No physical disk was read or written. No native installer, production installed
root admission, mutable-account authentication or installed-home support is
claimed by Phase 1.

Initial login and subsequent spawn now share bounded owned executable loading,
authoritative execute metadata and credential decisions. EXT4 copies executable
bytes under its filesystem lock; ELF loading occurs after that lock is released.
Detached root preparation, validation, publication and discard have explicit
ownership. Publication retains bootstrap backing and reuses the existing
`/dev`, `/run` and `/tmp` nodes; first process publication seals root attachment.

## Executed evidence

- `make test-root-phase1`: **8/8 PASS** (two ASan/UBSan host sector adapters,
  six BIOS/UEFI × SMP=1/4/8 guests).
  Evidence: `build/root-phase1/gate-lqg3lchb/manifest.json`, command log,
  serial logs, exact QEMU argv and Linux fsck logs in the same directory.
  Isolated workspace: `.codex-remote-attachments/ext4-phase9/guest-workspace-g455_yi5`.
  Baseline commit: `4c0720d76d21f1d89e745de005df0079221f544f`; the manifest and
  workspace inventory identify the tested working-tree sources by hash.
- Host gates exercise actual VFS/EXT4/JBD2, exact independent ELF byte comparison,
  preparation rollback, readonly admission rejection without writes, retained
  bootstrap handles, volatile routing and hidden placeholder contents, mount-point
  mutation rejection, trusted set-ID and external nosuid behavior, short reads,
  allocation/read failures, 1100-node cache churn, fully pinned saturation and
  complete allocation reclamation at simulated reboot.
- Every guest checks AP count, mandatory detached validation/publication,
  retained bootstrap backing and tty identity, cache churn/pinned saturation,
  snapshot failure cleanup, malformed ELF, all 13 existing spawn failure hooks,
  and 100 supervisor spawn/exit cycles after warmup. Quiescent resources match
  across PMM pages, VMM tables, heap blocks and kernel stack slots. Real login,
  shell, shadow denial, sudo and 20 subsequent shell spawns pass. Independent
  offline Linux fsck passes for all six root copies.
- `make test-elf-page-host test-perm-spawn-host`: **PASS**, including 16,388 ELF
  page cases, actual loader byte/permission/rollback checks, 32,768 set-ID vectors,
  actual secure-entry stack and sudo adapters. These also run inside the final gate.
- `make test-sudo`: **10/10 PASS**, retained at
  `build/permissions-phase4/sudo-s0psh8qr`. Live BIOS/UEFI password cases at
  SMP=1/4 plus non-wheel, passwordless, dropped-capability and external ext2
  nosuid controls preserve existing live behavior.
- Normal kernel strict build and `git diff --check`: **PASS**. The final runner
  verifies source stability and unchanged normal ISO/raw-image hashes.

## Boundaries and retained failures

All media are fresh regular-file fixtures. The root is validated then frozen for
existing readonly account consumers; homes remain under `/run`. This proves
executable loading and namespace plumbing, not production PARTUUID admission,
mutable installed accounts/homes, global shutdown dispatch or an installer.
Normal live build outputs and external `/mnt` nosuid semantics remain separate.

The dedicated guest fixture skips legacy user diagnostics before root publication
and unrelated SMP worker/TSC timing tests. Required BSP syscall MSRs, AP discovery
and scheduler startup remain. Snapshot faults are explicit test adapters, not
physical I/O error evidence. Host scheduler adapters do not establish IRQ safety.

Earlier failed runs remain under `build/root-phase1`: `gate-bl56drmx` exposed
missing fixture syscall initialization; `gate-ma__xfp1` exposed stale ELF-host
include/fault adapters; `gate-bzqud4mw` measured resources before supervisor
quiescence; `gate-8gdxst13` stopped in an unrelated TCG TSC tolerance check before
root mounting; `gate-ulkp3cbg` lacked the fake filesystem permission adapter.
The fixes are covered by the final unchanged-source matrix; these earlier runs
are not counted as passes.
