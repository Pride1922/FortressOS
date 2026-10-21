# PKG-2 account allocation bounds and admission profile

2026-10-21, continuation from `b283f7e`. Design and private host observations;
no allocator, boot, account ABI or installation behavior changes.
Builds on [account compatibility](PKG2_ACCOUNT_COMPATIBILITY.md).

## Result

A complete account update has a conservative **one-pass gross allocation** bound
of 220/80/35 filesystem blocks at block sizes 1/2/4 KiB, plus seven new inodes,
under the eligibility conditions below. These are 220/160/140 KiB respectively:
the existing 256 KiB/16-inode floor is larger. No increase to that production
floor is justified merely by the measured fixture deltas.

This closes the raw-space arithmetic question for one admitted pass, **not the
restart-invariant recovery guarantee**. It is not permission to reserve those
free resources twice or enable package installation. Journal/cache/heap/orphan
capacity, concurrent namespace changes and repeated interrupted attempts remain
independent constraints. Production A/H remain conditional candidates.

## Source-derived calculation

Let b be filesystem block size, D=ceil(8192/b), C=floor((b-12)/12).
The canonical extent rebuild in `ext4_mutate.inc:e4_rebuild` allocates:

```
T(n,b) = 0                                      if n <= 4
         L + (ceil(L/C) if L > 4 else 0)         otherwise
where L = ceil(n/C)
```

This counts new external leaf/index nodes, before freeing old tree nodes.
The engine separately rejects more than four indexes or 64 external new nodes.
For a directory with at most 1 MiB of allocated data, all mappings within EOF,
and no overlapping ranges, its worst extent count is at most 1 MiB/b.
The maxima of T over those counts are 14/4/1 nodes at 1/2/4 KiB.
Existing valid but noncanonical trees can contain additional old nodes; their
reclamation is not available allocation credit. The existing 64-old-node and
image/revoke limits must pass separately.

`account_redo_update` writes three complete stages, one temporary intent and
three complete publication temporaries: six files <=8 KiB and one 160-byte file.
Each is newly created, then written in one backend call, so even fully fragmented
allocation adds at most D data blocks and T(D,b) tree blocks per full-size file.
The intent needs one block and an inline extent. No allocation credit is taken
for the old canonical set, stale temporary cleanup, stage cleanup or tree frees.

Count every possible directory insertion, even when existing slack would suffice:

| Operation | Insertions |
| --- | ---: |
| Three stage creates | 3 |
| Temporary intent create | 1 |
| Temporary-intent to pending rename | 1 |
| Three publication-temporary creates | 3 |
| Three temporary to canonical renames | 3 |
| Total | 11 |

`ext4_write.inc:e4_dir_edit` either uses existing record slack or adds exactly one
directory data block and rebuilds its extent tree. Deletions do not allocate
directory storage. Charge each insertion one data block plus the maximum new
directory tree, independently of whether it actually grows. Thus:

`U(b) = 6D + 1 + 6T(D,b) + 11 * (1 + max_directory_T)`

| Block size | D | File T | Directory T max | U blocks | U bytes | New inodes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1,024 | 8 | 1 | 14 | 220 | 225,280 | 7 |
| 2,048 | 4 | 0 | 4 | 80 | 163,840 | 7 |
| 4,096 | 2 | 0 | 1 | 35 | 143,360 | 7 |

This is gross successful planner allocation, not the net committed free delta.
Aborted speculative work must not be counted as consumed durable grant credit.
Inode tables/bitmaps/group descriptors/superblock/journal are existing allocated
storage; modifying their images consumes transaction/RAM resources, not new
filesystem blocks. The formula does not pay for a larger journal or mount setup.

Pending account recovery creates only three publication temporaries and performs
six insertions. Its corresponding one-pass raw bound is
`R(b) = 3D + 3T(D,b) + 6*(1+max_directory_T)`:
117/42/18 blocks (119,808/86,016/73,728 bytes), three new inodes. This analytic
bound assumes authoritative stages/intent already exist and validate. It is not
an independently instrumented pending-recovery maximum in this gate.

## Candidate eligibility and rejection rules

These are prerequisites for a future package-admission validator; they are
**not implemented checks** in the current account service.

- Supported block size is 1/2/4 KiB on the already admitted healthy journaled
  installed root. Resolve and retain actual objects; no path-only identity,
  symlinks, foreign mounts, extra hard links or arbitrary redo roles.
- `/etc`, `/var/lib/fortress/accounts` and their validated ancestors already
  exist with expected ownership/modes. Missing setup directories make package
  eligibility fail before package intent. Existing account boot/setup behavior
  remains unchanged; U/R exclude its directory creation.
- Every affected parent has a valid bounded dense directory map: no data or
  unwritten allocations outside EOF, no holes in used directory blocks, supported
  tree depth/ownership and <=1 MiB directory size. Size alone is insufficient:
  the current account read adapter checks logical size, not this map profile.
  Rebuild canonicalizes **all** pending extents, including mappings beyond EOF
  if accepted elsewhere; those would invalidate the count used in the formula.
- Parent capacity must allow the accepted sequence, not merely satisfy byte/free
  bounds. A full 1 MiB directory can reject with EFBIG despite abundant free
  blocks. Validate exact record slack and growth feasibility using the same
  planner rules; revalidate or exclude competing namespace writers. Never change
  existing account handling just to force a package to pass admission.
- Stages/intent are bounded fixed roles. Reject malformed, foreign or conflicting
  account pending state for new package admission; perform account recovery first.
  Pre-existing stale files cannot be adopted by a package lease. Old allocated
  objects/pins remain charged to actual used storage, never to hypothetical frees.
- Before each accepted account operation, keep its entire remaining envelope
  outside durable package G. Preserve >=max(existing floor, required envelope)
  for ordinary/package spending. Fixed-role account use can consume that envelope
  but cannot spend G. The current raw-free preflight and bypass do not enforce this.
- Independently admit image/revoke/journal/RAM/cache/orphan resources. A file
  with sufficient free blocks can still exceed 64 images or orphan capacity;
  fragmentation across allocation groups affects bitmap/descriptor snapshot
  demand. These formulas do not prove every future transaction fits those pools.

Out-of-profile roots fail **package eligibility**, not existing account boot
admission. No grant record, mount hook or new rejection is added by this review.
None of the bounds covers arbitrary privileged namespace interference.

## Verification and exact evidence

Executed:

```
wsl -d Ubuntu-24.04 -- python3 scripts/test_pkg_account_bounds.py .codex-remote-attachments/pkg-account-ext4-20261021-121959
```

**PASS: 26 actual-code profiles under ASan/UBSan and Linux e2fsck.** Evidence:
`.codex-remote-attachments/pkg-account-bounds-20261021-124437/`.
The runner copies the retained source workspace, adds an observer after successful
negative `e4_account` deltas, and enables it only across `account_redo_update`.
It sums gross successful block/inode allocations, including directory and new
extent nodes, before frees. It asserts each observed update <=U and seven inodes.
Three-update pin profiles reset/verify the bound for each update, not for the
aggregate retained lifetime. Derived source hashes, logs and images are retained.

Small, byte-cap, retained-descriptor and directory-growth cases run across all
six block/sector geometries; fragmentation adds two 1 KiB cases. The arithmetic
enumerates every candidate directory extent count (1,024+512+256 = 1,792 counts)
to compute tree maxima. This confirms the source-derived formula, but the host
fixtures do not instantiate every maximum-sized/fragmented parent directory.
The bound assumes accepted backend progress; selected denial cases were tested
separately in the preceding checkpoint. Synthetic byte-cap input still bypasses
full userdb/service admission as documented there.

## What is still needed for a durable reservation

Do not multiply U by an unbounded retry count, or treat R as a fresh reserve
available on every reboot. A cut can retain new canonical bytes, partial files
or directory growth; those consume part of an earlier promise. Boot orphan
reclamation drops unlinked old generations but does not shrink grown directories
or reset consumed space. Exact slot reuse under repeated interrupted publication
needs proof; arbitrary interleaved `/etc` writers invalidate assumed slack.

Next specification should bind an independently protected account envelope to
the actual durable role/layout state and remaining publication steps. Prove
that reconstruction after replay yields sufficient **remaining** resources,
without re-crediting already allocated objects or consuming package G. Alternatively,
prove a stable publication-slot profile that makes directory growth bounded
across repeated cuts; that would be a separate reviewed namespace contract.
Neither follows from a successful one-pass or finite cut campaign.

Recommendation: retain 256 KiB/16 inodes as the existing production floor and
as the candidate one-pass minimum; do not publish a larger guessed constant or
freeze production A/H yet. Use U/R for the next pure admission/remaining-budget
model, then actual-code differential tests, before protected runtime integration.
Installation, production trust, root/grant provisioning and all mutation stay
disabled. No boot/physical acceptance, shared-media hash claim or release build
is added by this host gate.
