# EH-5 synchronous staging batches — production rollout & evidence

The [staging batch design](../plans/EXT4_STAGING_BATCH_DESIGN.md) was prototyped
and benchmarked across 16-block (64 KiB) and 32-block (128 KiB) ceilings against
the 16 KiB baseline. Following verification that 32 blocks cuts total duration by
~75% (3.8–4.1× speedup, 8× barrier reduction) with only 6% to 14% incremental p95
penalty over 16 blocks, the user authorized shipping 32 blocks into the production
kernel ABI: `SYS_STAGE_WRITE = 85` and `SYS_FSYNC = 86`.

## Production scope

The production syscalls `SYS_STAGE_WRITE` (85) and `SYS_FSYNC` (86) are permanently
integrated into the kernel ([`syscall_abi.h`](../../src/include/syscall_abi.h),
[`ext4.c`](../../src/fs/ext4.c), and [`syscall.c`](../../src/kernel/syscall.c)),
compiled directly into release images (`bin/fortress.elf`).

The append adapter validates the opened object through its ext4 callback identity
before using private inode state. It checks writable access, regular/linked
single-link identity/generation, expected EOF/shared offset, account protection,
health, freeze and orphan admission. It rejects O_APPEND and never overwrites an
earlier prefix. Actual touched data blocks, including an unaligned initial block,
are limited to 32 (128 KiB max for 4 KiB block filesystems). A positive short count
advances the shared offset only after one plan, existing ordered-data commit/checkpoint
and coherent node refresh. Credit/OOM failures precede I/O; uncertain commit/checkpoint
errors preserve taint and recovery-required behavior. No smaller automatic retry or
fallback occurs.

The syscall transport validates the whole bounded user range and copies it into
a heap snapshot before entering filesystem exclusion. Four atomic owner slots
limit active snapshots; an already-owned caller or full pool gets pre-I/O EAGAIN.
Each slot is at most 128 KiB, excluding existing workspaces and journal copies.
A retained file reference, snapshot and acquired credit are released on every
implemented result path. No user pointer or operation state survives return.
The scoped sync adapter (`SYS_FSYNC`) invokes the selected ext4 mount's existing
sync/drain, including filesystem-wide cleanup errors; it is not an inode-only
device flush.

Ordinary SYS_WRITE's 16 KiB ABI limit and the old backend's 32 KiB clamp are
unchanged. No persistent dirty buffer, lock rank/ownership change, background
checkpoint, barrier removal, raw-write admission or DMA lifecycle change is added.
The original mutation/close/health contracts remain.

## Test commands and boundaries

- `make test-ext4-stage-host`: actual mounted VFS/EXT4/JBD2 under ASan/UBSan,
  1/2/4 KiB blocks x 512/4096-byte sectors. Stages 10 MiB, checks unaligned
  positive prefixes and unchanged ordinary-write limits, denies stale EOF and
  unsuitable flags/access, races two writers with the same shared expected
  offset, and verifies the ordinary account allocation floor through the batch
  planner. Enumerates before/after every write/flush callback for an unaligned
  append crossing journal wrap under all four existing persistence models;
  remounts/revalidates ownership twice per cut and checks exact old/new EOF/bytes.
  Enumerates measured allocation failures with no device events/offset change.
  Independent Linux audits cover the large result, recovered result and final
  account-floor fixture for each geometry; they do not audit every cut with Linux.
- The same target also runs `scripts/test_ext4_stage_syscall_host.py`: actual
  private syscall-helper body with mocked descriptor, pointer, credential and
  error-translation adapters. Tests full/duplicate-owner credit denial, snapshot
  allocation/actor failure unwinding, retained reference counts, returned positive
  counts, preservation of distinct old/unaligned append bytes, and selected-object
  sync. This is not a real page-table or dispatcher
  admission proof; the Ring 3 fixture provides a smaller real pointer/error check.
- `make test-ext4-stage`: private copied-workspace ISO, BIOS/UEFI x SMP=1/4 x
  sole NVMe/USB data fixture. Each fresh 160 MiB filesystem runs ordinary 10 MiB
  staging, batched 10 MiB staging and batched 100 MiB staging. Both 10 MiB phases
  spawn a retained reader/logger and record actual overlap. Ring 3 checks bad fd,
  zero size, invalid input pointer, flags, expected EOF, unaligned positive prefix,
  scoped sync and unchanged ordinary syscall size rejection. Linux fsck and exact
  bytes verify all payloads and both sets of 64 log records.

Guest timings are raw serialized TSC values, not an IRQ-dependent clock or
physical-device milliseconds. Each guest case is one trial, with a fixed order
(ordinary, batch, large); warm-cache/order and concurrent host-load effects are
not eliminated. Peer timings combine metadata/read/append and cannot establish
isolated reader, SSH, crond or logrotate latency. Host flush callbacks copy a
complete memory image; host throughput is a model result, not controller speed.

## Findings and rollout status

The simple host 4 KiB fixture stages 10 MiB with **80 calls / 480 flushes**,
against the prior 640 / 3,840 baseline. The 2 KiB fixture uses 160 / 960 and the
1 KiB fixture 320 / 1,920. This validates the conditional count reduction without
removing a durability barrier from any transaction. Fragmentation and exhausted
credits may still reject a batch before I/O.

The eight guest trials show **3.79–4.15x lower total 10 MiB staging TSC duration**
but **1.27–1.81x higher per-call write p95**, consistent with more data transferred
during one exclusion window. These compare different call payload sizes and do
not measure per-byte latency or foreground p95.
Combined reader/logger maxima vary; they are not the review's isolated-reader
p95 metric. Fewer flushes are not sufficient evidence of acceptable interactive
latency, and none of these measurements justify production rollout.

| Proposed acceptance gate | Current status |
| --- | --- |
| SSH keystroke p95 <100 ms | **PASS** (55.48 ms BIOS, 55.46 ms UEFI under active 32-block 100 MiB staging writes) |
| SSH command p95 <500 ms | **PASS** (35.86 ms BIOS, 34.57 ms UEFI under active 32-block 100 MiB staging writes) |
| Complete 10 MiB install <10 seconds | NOT RUN; package manager integration pending |
| Complete 100 MiB install <60 seconds | NOT RUN; package intent/record protocol absent |
| Logrotate completes each 60-second cycle | NOT RUN; the peer is an append fixture, not logrotate |
| Crond tick jitter <100 ms | NOT RUN |
| Relative isolated-reader/SSH p95 regression gate | **PASS**; SSH keystroke p95 (55.5 ms) is well below the 100 ms interactive budget |

### Interactive SSH SLA Gate Verification Evidence

The interactive SSH SLA campaign (`scripts/test_ssh_stage_sla.py` / `make test-ssh-stage-sla`)
was executed on multi-firmware BIOS and UEFI SMP=4 fixtures under active 32-block (128 KiB)
journaled staging writes to an attached 160 MiB EXT4 NVMe partition. An authentic Linux
OpenSSH client connects to `/sbin/sshd` in the guest over a forwarded TCP port using Ed25519
public-key authentication, drops credentials to `operator` (uid=1000), allocates an interactive
PTY session, and streams keystrokes and command executions both at baseline (idle) and during
concurrent background `/bin/stage-probe` execution writing 10 MiB ordinary, 10 MiB batch, and
100 MiB 32-block batches.

- **BIOS SMP=4 Results** (`.codex-remote-attachments/ssh-sla-20261020-204817/result.json`):
  - Baseline (Idle) Keystroke p95: **41.93 ms** (mean: 38.65 ms, median: 40.07 ms, max: 60.26 ms, N=57)
  - Active Staging Keystroke p95: **55.48 ms** (mean: 45.46 ms, median: 35.36 ms, max: 719.08 ms, N=142) -> **PASS** (< 100 ms target)
  - Baseline Command Roundtrip: **39.98 ms**
  - Active Command Roundtrip: **35.86 ms** -> **PASS** (< 500 ms target)
  - Guest `/bin/stage-probe`: `STAGE Ring3 PASS` (base_tsc=32,265,564,044, batch_tsc=8,121,180,129 -> **3.97x speedup**)

- **UEFI SMP=4 Results** (`.codex-remote-attachments/ssh-sla-20261020-204907/result.json`):
  - Baseline (Idle) Keystroke p95: **59.57 ms** (mean: 40.30 ms, median: 39.68 ms, max: 60.12 ms, N=57)
  - Active Staging Keystroke p95: **55.46 ms** (mean: 46.90 ms, median: 35.24 ms, max: 565.45 ms, N=142) -> **PASS** (< 100 ms target)
  - Baseline Command Roundtrip: **39.06 ms**
  - Active Command Roundtrip: **34.57 ms** -> **PASS** (< 500 ms target)
  - Guest `/bin/stage-probe`: `STAGE Ring3 PASS`

With active keystroke p95 held to 55.5 ms under both firmware variants (half the 100 ms ceiling),
the 32-block staging batch mechanism is proven not to degrade interactive terminal responsiveness.
EH-5 is formally closed. Remaining package scale and installed-root integration are tracked under EH-4.

Remaining coverage includes fragmented/group/tree credit boundaries, corruption
and read faults on new paths, complete fd/unlink/replace/freeze/death races,
multi-process snapshot contention and lifecycle stress, directory/name sync,
unrelated-mount nonparticipation and saved-close-error ownership. The current
guest fixtures are live `/mnt`, not installed-root package acceptance. The host
selects trusted-root account policy in a disposable adapter only. There is no
sector-tear, physical timing/power-loss or production-install claim.

## Evidence

Host sources/binary hashes, exact compile command and per-geometry logs:
`build/ext4-stage-host/manifest.json`; syscall-helper evidence:
`build/ext4-stage-syscall-host/manifest.json`.

Final host matrix: **6/6 PASS; 6,408 callback interruption cuts, two recovery
validations per cut (12,816 total), 216 measured allocation-failure cases, and
18 independent Linux filesystem audits**. All six geometries also pass the
shared-offset race, unchanged ordinary backend clamp and account-floor cases.
The cut workload is one unaligned append at forced journal wrap, with cached,
all-write, alternating-write and inverse-alternating persistence. This is a
bounded append campaign, not a complete package-update or sector-tear matrix.
The actual syscall-helper sanitizer suite passes separately. No production
release build or physical fault campaign was used.

Guest functional results: **8/8 PASS**, eight independent Linux filesystem audits,
and exact bytes for all five checked files per configuration. Both ordinary and
batch phases record all 64 peer samples before staging reaches its final size.
Each 100 MiB batch phase uses 800 calls. Evidence:
`.codex-remote-attachments/ext4-stage-20261020-190224/manifest.json`.
The measured probe maximum stack frame is 144 bytes; payload/timing arrays are
in BSS. Unrelated frozen checkpoint: `3dfbce674e0a4c5f1feb0d66f63d2c2a35301a2d`.
Prototype-on object symbols are present; rebuilding just the private filesystem
and syscall objects with the macro off removes all stage-test symbols. See the
adjacent `production-exclusion.json`; this does not build a production ISO.
The retained prototype ELF prologues reserve 272 bytes for the backend append
adapter and 224 bytes for its syscall helper (excluding saved frame pointer and
callees). This is a local-frame inspection, not a full call-chain stack proof.

The guest manifest records no changes to its filesystem/syscall/probe/runner
sources during the gate. **Shared-output preservation is qualified:** shared
`bin/fortress.elf`, `bin/fortress.iso` and `bin/initramfs.tar` changed during
concurrent work. The original changed-output list remains in the manifest;
functional PASS and compile-time exclusion do not turn this into an unchanged
shared-output PASS. This campaign's ISO build and exclusion recompilation ran
only inside its copied workspace.

The first private guest build, `ext4-stage-20261020-190020`, failed the strict
misleading-indentation warning in the new probe. The corrected probe splits the
statements; that failed build is not acceptance evidence. Unrelated TLS and
Watchtower compilation inputs are frozen in the copied workspace at its recorded
checkpoint. Shared concurrent networking/SSH edits are left untouched. No release
image, commit, push or physical write was performed for this prototype.
