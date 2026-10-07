# EXT4 Phase 9.4 — Disposable guest crash execution

Declared finite guest gate PASS on 2026-10-06: 66/66 cases and evidence review.
Production journaled RW remains
disabled. No filesystem, driver, locking, cache, DMA or mount-policy code is
changed by this phase; it adds isolated guest runners and evidence review.

## Execution and declared coverage

Source snapshot `guest-workspace-2chvkmiv` was built independently of shared
`build`/`bin` outputs. Each campaign copies its ISO and matching debug ELF,
uses owned GPT/NVMe regular files and paired OVMF variables, records QEMU argv,
and reaps the stopped QEMU process. The debugger reads registers/memory and
uses hardware breakpoints; it does not patch guest instructions or memory.
DWARF supplies writer field offsets from the actual ELF.

The declared finite matrix contains 66 cases:

- 36 transaction cases: BIOS/UEFI × 1/2/4 KiB × SMP=1/4 × before ordered
  write submission, durable commit before checkpoint, and one completed
  checkpoint home write. `guest-crash-yw2l_qvc` passed 36/36.
- 12 recovery cases: the same firmware/geometry/CPU combinations, interrupted
  after one replay home write while the mount is still unpublished.
  `guest-crash-x2gv56t2` passed 12/12.
- 12 AP append cases: BIOS/UEFI × 1/2/4 KiB × independent/shared handles,
  SMP=4. Interrupt the first durable AP record before checkpoint, restart
  before the next append truncation, and verify the same exact 16 bytes with
  Linux. Final normal execution requires both files to contain exactly the
  200 distinct records, 3,200 bytes each. `guest-append-crash-o3zzmita`
  passed 12/12; supplementary final Linux record audits passed for every case.
- Six open-unlink cases: BIOS/UEFI × 1/2/4 KiB. Interrupt durable unlink
  while independent and shared descriptor references are live. Linux and
  a fresh FortressOS mount must reclaim the orphan and preserve the earlier
  renamed/truncated file, followed by clean sync/shutdown.
  `guest-orphan-crash-_w5y533z` passed 6/6. Independent Linux and fresh guest
  recovery agree exactly on allocation bitmaps and free block/inode counts.

For each crash, Linux journal-only replay on a separate copy precedes
`e2fsck -fn` and exact byte/namespace checks. Fresh guest recovery is observed
before the fixture can rewrite the result. Final normal boots exercise sync,
further mutation, shutdown freeze and independent clean-state/fsck checks.

Normal namespace/truncate/pin/reuse and true AP append regressions also passed
six BIOS/UEFI geometry cases, 12 boots at SMP=4:
`guest-smp4-dabvp_r2`. That regression includes the wrapped 2 KiB fixture;
the crash matrix uses normal journal placement.

## Reproduction and review

`make test-ext4-guest-crash EXT4_GUEST_WORKSPACE=<prepared-isolated-workspace>`
executes the four campaigns without rebuilding shared artifacts. Prepare and
build the workspace with `scripts/create_ext4_guest_workspace.py` first.
`scripts/review_ext4_guest_crash.py` checks all four completed directories,
exact label coverage, identical binaries, retained byte results, replay
publication state, AP debugger thread identity and final AP record sets.
The review passed with no errors; its digest ledger is
`phase9-4-review.json`. Python compilation, Makefile dry run and
`git diff --check` passed. The review checks saved argv against the sole NVMe
fixture and paired firmware; the runners reject extra drive, blockdev,
snapshot and legacy disk arguments before launching.

Evidence lives under `.codex-remote-attachments/ext4-phase9`, outside build.
Verified archival copies are in `verification-oek_6rju`, including all four
campaigns, normal regressions, the first vertical case and the failed AP
attempt. Images are compressed and decompressed hashes checked; originals
are retained. The exact isolated build source snapshot and binary identity
are checked by `scripts/retain_ext4_guest_workspace.py`, separately from
the current runner-source snapshot.
The first AP attempt `guest-append-crash-9ufbd9pa` failed its debugger thread
identity assertion. It is retained. The runner now selects the stopped thread
from the remote stop packet before reading registers; CPU/thread identity
is required for an AP pass. This was a harness observation failure, not a
filesystem byte mismatch. Raw readelf warnings are retained separately from
the parsed complete writer layout.

## Limits

These are finite native guest milestones, not all transaction instructions or
all interruption pairs. Killing QEMU retains the host page cache; writeback
cache is recorded explicitly, and this is no physical power-loss or torn-write
claim. Cache-loss/reorder/tear and write/flush failure evidence remains the
separate host campaigns. SMP=4 boot configuration alone does not prove AP
mutation: early transaction/recovery cases execute on the BSP; the append
cases separately require a stopped AP debugger thread.

No new journaled USB or physical acceptance is claimed. Accepted E4-A physical
cache evidence, non-journaled E4-A, default ext2 and protected contracts remain
unchanged. Phase 9.5, physical acceptance and production rollout remain separate.

The phase adds no new guest failure-injection backend. Failed recovery/write/
flush admission and publication assertions remain the host evidence from
9.2–9.3; these guest cases establish native recovery of supported crash inputs.
