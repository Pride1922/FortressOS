# EXT4 Phase 9.2 — Mounted crash campaign

Execution specification, 2026-10-05. Production journaled RW remains disabled.
The immutable Phase 9.1 inventory is `foundation-5aprl4_q` under
`.codex-remote-attachments/ext4-phase9`.

## Declared primary matrix

Use all 156 operation traces: create, mkdir, extending write, append, rename,
truncate, unlink, open-unlink, last close, rmdir, reuse, sync and freeze across
1/2/4 KiB blocks, 512/4096-byte sectors and normal/wrapped placement.
The frozen six persistence profiles and tear offsets are defined by
`scripts/ext4_crash_campaign.py`; their schedules remain unchanged from 9.1.

| Schedule | Declared cases |
| --- | ---: |
| Every write/flush, before and after, six profiles | 116,832 |
| Eleven sector-prefix tears, cached and write-through | 187,792 |
| Four interrupted-flush subsets, six profiles | 28,800 |
| Total | 333,424 |

Every schedule has a ledger identity, including byte-identical stable images.
Within each immutable preimage, identical stable bytes share one actual fresh
mount; no filesystem, descriptor, allocation or cache state survives between
unique images. Report the unique recovery count separately from schedule count.

## Outcome and independent checks

A separate Python record/checksum parser derives the durable transaction prefix
from stable media and actual captured transaction images. Recoverable inputs
must mount and expose exactly that namespace and bytes. Detectable corruption
needs a named independent on-media witness; arbitrary rejection does not pass.
Preflight rejections must publish no mount and issue no writes or flushes.

After recovery, freeze must drain orphans and leave an empty journal and clean
filesystem. Identical complete home-sector images share an independent Linux
audit only after those assertions pass. Linux checks exact bytes, complete
namespace, link counts, disjoint ownership, orphan absence and unmounted
`e2fsck -fn` exit 0. No repair is used to make a result pass.

The sparse persistence model must match the original full-image model on all
552 calibration schedules. Independent controls distinguish old/new prefixes,
journal checksum corruption and stale checkpoint metadata. Preserve the 9.1
Linux stale-byte and bitmap-disagreement controls.

## Extended coverage

Build six additional fixtures through actual mounted mutation: three one-block
file extents separated by allocated blockers, followed by blocker deletion.
The starting bytes remain exactly three blocks of `O`. Verify the three extent
entries and clean Linux ownership before inventory. Inventory all operations
again, then apply every frozen schedule to extending writes in all 12 geometry
and placement combinations. Require inode-root promotion on the resulting
five-extent file, not merely a byte-correct contiguous allocation.
This separate immutable inventory (`foundation-4j8k_43_`) declares 7,800 atomic,
12,716 tear and 1,728 interrupted-flush schedules: 22,244 in total.

Retain and rerun directory growth/rename, supported depth-2 mutation and cleanup,
maximum-credit and exhaustion checks, shared offsets, independent append,
staging/OOM/read/write/flush failures, and taint checks. Their focused legacy
fault schedules are separate evidence from the six-profile mounted campaign;
do not add their counts to the primary 333,424.

## Retention and limits

Use immutable compressed preimages plus event payloads and reproducible sector
deltas, retaining full failing inputs. Verify every declared ledger identity,
sample deterministic input reconstruction hashes, and every retained Linux
audit delta. Retain source/binary hashes, source snapshots, exact commands and
raw logs outside `build`. Worker deadline is 180 seconds, each 2,000-case batch
is bounded by 300 seconds and each campaign retains less than 8 GiB.

This is a finite host campaign, not all histories, physical power-loss evidence,
guest acceptance or production activation. Recovery-interruption histories and
interoperability extensions belong to 9.3; guest/USB/physical gates remain later.
Latest-version sector reordering does not model persistence of superseded
historical sector versions. Visible-data overwrite atomicity is not promised.
