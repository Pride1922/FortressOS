# ext4 boot allocation-scan optimization — 2026-11-03

## Checkpoint

Before this change, all current source work was pushed as `c1b04ee`, including
ACPI EC battery parsing (`cd1a8c9`) and Vault/tabbed terminal (`c1b04ee`). The
remote tag `checkpoint/2026-11-03-before-ext4-boot-scan` preserves that boundary.
The source tree was clean there. A checkpoint records source; it does not claim
physical acceptance of the combined features or replace the published installer.

## Problem and change

Allocation admission counts every block/inode bitmap bit through `e4_bit()`.
Ownership validation also counts eight bits separately for each compared byte.
Several per-group reservation checks rescan ranges belonging to earlier groups.
These costs grow with filesystem capacity, including on a mostly empty root.

`ext4_bitmap.h` now counts allocated bits with portable scalar integer operations
per byte. Full bytes and the last partial byte remain bounded to the exact bit
count; padding bits are ignored exactly as before. There is no POPCNT/SIMD
requirement, table initialization, compiler optimization change or extra storage.
The ownership comparison still checks every bitmap byte, including the existing
one-bit offset for 1 KiB filesystems, and still checks partial final-group bits.

`e4_admit()` remembers the first reservation inserted for the current group.
Earlier groups' range starts are necessarily below this group's start, so those
ranges could not pass the old start-in-group filter. Only the current group's
new ranges need that same bitmap check. Descriptor geometry and overlap rejection
remain unchanged.

After admission has sorted reservations, journal allocation and lazy-group
ownership validation find the first start in the current group by lower-bound
search, then visit all starts before its end. This preserves the previous
`start <= range.lo < end` selection; it does not change lazy bitmap synthesis,
metadata range lengths or allocation rules.

Both journal-preview and reopened-filesystem validation still run. Every checksum,
free counter, inode/reference/extent ownership test, orphan/replay rule, write
exclusion, flush barrier and metadata cache-coherence rule remains. No filesystem
format or public ABI changes. Capacity-dependent validation still exists; this
fix reduces its CPU overhead without weakening admission.

## Controlled measurements

The probe compiles the actual VFS/ext4/JBD2 code at `-O0`, matching the kernel's
optimization level. Baseline is checkpoint `c1b04ee`; the after worktree differs
only in the scan implementation and filesystem-host input adapters. Both use the
same published factory root and the actual installer expansion code. Each run
gets a fresh regular-file copy; three alternating before/after pairs are pinned
to the same host CPU. Linux `e2fsck -fn` passes after all twelve clean freezes.

| Geometry | Before median mount | After median mount |
| --- | ---: | ---: |
| 256 MiB, 2 groups | 40.072 ms | 40.004 ms |
| Dell-sized 255991291392 bytes, 1908 groups | 1674.862 ms | 1206.987 ms |

The large fixture improves by about 28%. All six large runs perform exactly
16286 backend reads, read 66655232 bytes, and checksum 37811388 bytes. Small runs
likewise preserve request/read/checksum counts. These are host mount timings,
not Dell boot times or installation timings.

Evidence: `build/ext4-boot-scan/paired.json`, `paired.log`, per-run JSONL and fsck
logs. Private before/after probes and source snapshots are under
`.codex-remote-attachments/boot-scan-{before,after}/build/perf/`.

## Verification and limits

- `make test-ext4-bitmap-host`: exhaustive 256-byte values and all bit tails,
  10000 randomized unaligned exact-length maps under ASan/UBSan, padding-bit
  invariance; reservation lower-bound search versus a linear oracle over every
  boundary/gap, empty/full tables and `UINT32_MAX`.
- Strict production kernel compilation and link pass.
- Full mounted VFS/replay/fault/clean-freeze matrix: **PASS 12/12** across
  1/2/4 KiB filesystem blocks, 512/4096-byte sectors and normal/wrapped journal
  positions. All twelve native executables exit zero under ASan/UBSan;
  **16480 persistence cuts** plus read/allocation failure sweeps, invalid replayed
  ownership rejection, lifecycle/freeze checks and eight concurrent write/freeze
  races per profile pass. **72 independent Linux journal/fsck/byte audits** pass.
  Receipts: `build/ext4-boot-scan/final-result.json` and
  `build/ext4-boot-scan/mount/host-pchjjk9i/manifest.json`.

The standalone mount harness needed the current DAC/credential dependencies and
host input-device no-ops. Host thread adapters must precede kernel headers; the
initial input-symbol/link and include-order failures remain in the logs. The
standalone fixture excludes the unrelated package executor; production package
admission stays compiled into the normal kernel. No production checks were
removed to make the harness pass.

No new physical or guest firmware acceptance is claimed by the host measurements.
The published USB installer is unchanged by this source checkpoint/fix task.


Reproduction commands for the focused and full host suites:

```sh
make test-ext4-bitmap-host
FORTRESS_EXT4_HOST_JOBS=12 make test-ext4-mount-host EXT4_MOUNT_EVIDENCE=build/ext4-boot-scan/mount
```

The matrix's default concurrency remains three; the explicit override accepts
1–12 independent workers. An initial three-worker run was retired before
completion when scheduling the complete matrix concurrently. Its partial logs
are retained and are not claimed as a passing full run.


The original fixed 900-second per-case host deadline did not account for twelve
concurrent CPU-heavy sanitizer cases. The coordinator was paused while the
existing independent children continued; all twelve zero exit statuses were
retained, checked and reaped on resume before the independent Linux audits.
`build/ext4-boot-scan/supervisor.json` records this scheduling qualification.
The runner now scales the per-case scheduling allowance with the worker count;
its default three-worker/900-second behavior is unchanged. No injected failure,
filesystem assertion, driver deadline or production validation was disabled.

Other chats continued changing desktop/platform/timezone source after the
checkpoint. This report's ext4 measurements and host proof remain tied to the
recorded filesystem source snapshots; they do not claim acceptance of those
later unrelated changes. The ext4 follow-up commits only its own files.
