# PKG-2 recovery space: source audit and proposed direction

2026-10-21. Design review at HEAD `a6cdffa0af271d5547f4321dfd0c68eb484d73e3`.
No implementation, syscall assignment, mount hook, allocator exception or package
mutation is authorized here. Complements [the transaction review](PKG2_TRANSACTION_REVIEW.md).

## Finding and recommendation

The current PKG protocol does **not** establish allocation-free post-intent
recovery. There are concrete allocator paths, not merely missing benchmarks.
Recommend developing a **persistent, mount-admitted recovery grant**, with all
publication objects prepared before intent and recovery reduced to publication
and retirement wherever possible. This is a proposed direction, not an approved
boot or account contract. Do not enable installation from this review.

The grant addresses competition for filesystem blocks/inodes. It does not promise
progress through I/O failure, corruption, arbitrary privileged edits, expired
authorization, pinned pools or memory exhaustion. Resource admission and the
account-priority policy below remain required decisions before implementation.

## What the source actually does

References name functions so they remain searchable after line-number changes.
These are source-derived findings; no new fault campaign was executed.

| Recovery operation | Actual path | Space/resource consequence |
| --- | --- | --- |
| Install command into absent name; first inventory publication | `ext4_mount_journal.inc:e4_jrename_account` -> `ext4_namespace.inc:ext4_engine_namespace_rename` / `e4_rename_plan` -> `ext4_write.inc:e4_dir_edit` | Insertion searches existing record slack; otherwise `e4_initialize` allocates a directory data block and `e4_rebuild` can allocate extent nodes. A durable source inode does not reserve destination directory capacity. |
| Replace existing command or inventory | `ext4_replace.inc:e4_jreplace` -> `e4_replace_activate` / `e4_replace_entry` | Edits existing directory records; activation allocates no filesystem data blocks/inodes. It allocates a fresh cached node with `kcalloc`, requires cache room, adds an orphan, and stages journal snapshots. This is narrower than allocation-free recovery. |
| Recreate missing activation copy or rewrite a record | `ext4_transaction.inc:e4_file_write_actor` -> `ext4_write.inc:e4_file_plan` -> `ext4_mutate.inc:e4_rebuild` | Data holes allocate; even initialized extents can require newly allocated external tree nodes when the canonical extent list exceeds four entries. Old nodes are released after new nodes are planned; their future release is not spendable free space. |
| Remove command/pending; reclaim victims | `ext4_namespace.inc:ext4_engine_namespace_remove` -> `ext4_orphan.inc:ext4_engine_orphan_unlink` / `e4_orphan_cleanup` / `e4_orphan_edge` | Removal uses existing records; right-edge shrink frees storage without reallocating the extent tree. Orphan insertion is bounded to 64; pinned victims delay reclaim. Journal and memory resources are still necessary. |
| Sync selected mount | `ext4_mount_journal.inc:e4_journal_drain` | Drains eligible orphans and barriers; pinned orphans can remain. Saved cleanup errors and device failures are reported. Sync cannot manufacture reserved storage. |
| Commit every metadata operation | `ext4_transaction.inc:e4_semantic_commit` / `e4_transaction_commit_ordered` -> `jbd2_write.inc:jbd2_writer_begin` / `jw_stage` | Engine has 64 combined images; writer checks bounded log capacity and allocates a heap copy for each staged image. Copies are freed at checkpoint/abort, so earlier success does not reserve later staging memory. |
| Admit allocations | `ext4_mutate.inc:e4_account`, `e4_allocate`, `e4_reserve_inode_unlocked` | Staged counts preserve the existing account floor on ordinary trusted-root allocations once accounts are mutable. No package grant is recorded or subtracted. |
| Boot installed root | `ext4_mount_journal.inc:e4_mount_journal_admitted`; `installed_root.c:validate_tree` | Replay/ownership validation, journal activation and orphan recovery precede detached-root validation; `accounts_prepare` runs before root publication and can create/write/rename. Waiting until login or a manual pkg command would admit a grant too late. |

Mounted VFS dispatch reaches these helpers through the rename/replace callbacks;
they are not merely standalone host workbench APIs. Some Phase-8 comments in
`ext4_engine.h` / `ext4_transaction.inc` still say production mounts are disabled.
The public `ext4.h` APIs and current dispatch supersede that historical wording;
this review does not change runtime behavior to match stale comments.

Existing EH-2 evidence in [ext4-atomic-replace.md](../roadmap/ext4-atomic-replace.md)
reports replacement and cleanup at zero free data blocks, plus heap, journal and
pinned-cache failures. That supports the replacement row, not all PKG recovery.
Those historical tests were read, not rerun here.

## Counterexamples retained for acceptance fixtures

1. Prepare all payload/state sources, publish durable intent, then let unrelated
   directory creators consume the remaining suitable slack in `/usr/bin` and logs
   consume free blocks down to the account floor. Recovery's no-replace command
   rename reaches directory growth and returns ENOSPC. It needs no new command
   inode, but still needs directory storage. Checking slack before intent is
   insufficient without an exclusion surviving process death and reboot.
2. Reconstruct an activation copy from authoritative stage after intent. An
   initialized fragmented file with more than four canonical extents reaches
   `e4_rebuild` and needs new extent-tree blocks. Preallocating file byte length
   alone is insufficient. Overwriting published data also is not an atomic
   inventory-publication substitute.
3. Use only existing-file replacement, but retain all 1,024 cached nodes or fill
   64 orphan slots, or fail `jw_stage` allocation. No free-block allocation is
   needed, yet recovery can still stop. Persistent disk credit cannot fix this.

These are analytic failure paths to reproduce, **not new observed test failures**.
If activation/state source is consumed by a committed rename, recovery should
recognize exact published bytes/metadata and advance; it should not automatically
recreate that source. Unexpected absence of both source and valid destination
remains corruption/interference, not permission to reconstruct arbitrary objects.

## Alternatives

| Direction | Needed to make its claim true | Assessment |
| --- | --- | --- |
| Current preflight + volatile session | Survive death/reboot and fence every competing allocator | Insufficient as implemented/proposed; free counts are not ownership. |
| Strict allocation-free publication | All data/records durable before intent; exact absent-name capacity protected persistently; no post-intent writes/reconstruction; reserved RAM/cache/orphan/log resources | Plausible redesigned subset, not established by existing APIs. A placeholder at a new command name changes pre-intent namespace behavior and violates absent-name installation. A temporary slot does not protect the actual absent name across reboot. |
| Persistent grant + prepared objects | Atomic durable spend/replay, mount admission before allocating consumers, account compatibility, bounded recovery plans and namespace protection | Recommended direction for the current install/update/remove scope. Greater kernel/mount work; must undergo protected-contract review. |

An upgrade-only variant with existing command and inventory destinations could
avoid filesystem allocation during publication. It would exclude initial install,
still need persistent namespace integrity and volatile resource admission, and
would not satisfy the full plan. No silent scope reduction is recommended.

## Persistent-grant contract to specify before coding

Use one kernel-validated transaction identity on the admitted installed mount.
Grant units are filesystem blocks/inodes, never raw sectors or arbitrary extents.
A bounded protected record must bind mount/repository identity, transaction,
generation, phase and remaining budget to exact durable intent/state identities.
Its bytes, update mechanism, protected namespace and parser are not frozen here.
A checksum is corruption detection, not authority against privileged alteration.
Do not interpret a user-editable reservation filename as an allocator capability.

The critical invariant, for each resource independently, is:

`ordinary allocation: free_after >= account_floor + remaining_package_grant`

`tagged package allocation of d: free_after >= account_floor + (grant_before - d)`

Require `d <= grant_before`, checked arithmetic and full-plan validation before
I/O under existing filesystem exclusion. Already allocated stage blocks are not
also counted in the remaining grant. For example, free=100, floor=20, grant=30
allows ordinary use down to free=50. A package spend of 5 must atomically leave
free=45 and grant=25 if starting at that boundary; neither value can be replayed
without the other. The numbers illustrate accounting, not proposed quota sizes.

Grant spending and its filesystem allocation must be in the **same journal
transaction** or have a separately proved equivalent recovery protocol. A later
ordinary file rewrite of the grant is insufficient: a cut can double spend or
lose accounting. The protected grant update itself needs bounded preallocated
storage and journal images; it must not recursively allocate to account for an
allocation. Any authorized identity check must fit the bounded plan as well.
Abort restores provisional credit; uncertain commit fences activity and replay
selects the durable outcome. Cleanup never refunds guessed file sizes. Reconcile
actual owned frees, and either retain surplus conservatively or publish a proved
budget reduction; no double refund across repeated cleanup.

Durably protect the grant **before intent can become durable**. Owner death after
that point retains protection; an abandoned pre-intent grant is reconciled under
exclusive admission, never expired by PID/time. Keep it until pending retirement
is durably established. A cut between pending removal and grant retirement may
conservatively retain surplus; it must never admit a second transaction or strand
remaining required cleanup. Final retirement requires exact state/command and
pending identities, not just ENOENT. Define grant-without-intent and intent-without-
grant states explicitly; malformed/ambiguous records stop writable admission,
never reset the grant to zero and continue ordinary allocation.

Budget from the actual accepted layout/planners, including directory growth,
extent fragmentation, record/intent storage, authoritative and activation copies,
metadata updates and maximum simultaneous retained generations. Do not publish a
constant worst-case grant until all accepted geometries/layouts have a bound.
Unbounded root namespace changes can invalidate a bounded plan even with free
blocks: protect package ancestors/objects and define the privileged-writer
boundary. Session ownership alone does not do this after death/reboot.

## Account priority and boot boundary are unresolved contracts

Follow-up: [account compatibility candidate](PKG2_ACCOUNT_COMPATIBILITY.md)
defines independent account headroom, effective free-minus-grant admission,
priority and boot/failure ordering. Its actual-redo/modelled-space host probe
passes; an actual EXT4 envelope and approval of changed admission semantics
remain required. No runtime behavior is changed by that candidate.

The current fixed-role account callbacks set `account_allocation=true` and bypass
the ordinary floor. A package grant added only to the ordinary check would be
consumable by those callbacks. The 256 KiB/16-inode floor is not a proven upper
bound on every account recovery workload; do not infer one from the constants.

Before admitting any package intent, prove bounded account recovery headroom
separate from the package grant and specify how all fixed-role allocations
preserve that grant. If that cannot preserve existing account priority, reject
package admission or redesign the reservation. Do not silently restrict account
recovery, lend its floor, or promise both unbounded account priority and guaranteed
package progress on a finite filesystem. This is a blocking review decision.

Proposed boot boundary: reconstruct and validate grant state from replayed
filesystem state before `accounts_prepare` or any other allocating detached-root
consumer, and before root publication. Current initial orphan shrink does not
allocate filesystem storage; nevertheless reconcile it against authoritative
grant/object identities before trusting the recovered budget. Replay preview and
existing ownership validation must remain intact. A loader, login, Watchtower or
manual-command-only hook is insufficient. Exact placement and failure routing
require explicit approval under PROTECTED.md; no hook is added here.

Manual package roll-forward can remain a later explicit command, provided the
kernel already protects the admitted grant and namespace. Early reservation
admission and automatic package recovery are different decisions. Invalid grant
state must route to the independently authenticated recovery path without
publishing a writable installed root; do not log onto that root before admission.
Roots with no package metadata must retain current behavior; distinguish absence
from unreadable, malformed or partially provisioned metadata using a reviewed
provisioning marker/format, not a permissive missing-file fallback.

Follow-up: [durable grant and mount-admission candidate](PKG2_DURABLE_GRANT_DESIGN.md)
specifies candidate encoding, provenance, atomic accounting and early mount
ordering. Numeric planner bounds and production integration remain unresolved.

## Acceptance gates and sequence

All gates below are **NOT RUN**; this is a specification for subsequent work.

1. Freeze account compatibility, protected grant ownership/encoding, namespace
   boundary and mount failure routing; approve those protected contract changes.
   Then specify atomic spend/retirement and bind the pending grammar to it.
2. Add actual-code host reproductions of the three counterexamples. Record
   precise pre-I/O failures and unchanged storage; preserve failing baseline
   evidence. Add a planner audit for every accepted operation/geometry and reject
   layouts exceeding bounded blocks, inodes, images, revokes, directory traversal
   or orphan capacity before intent.
3. Host sanitizer adapters: exact allocation bitmaps and grant totals after every
   spend/abort/replay; competing logs, account recovery and directory growth;
   block and inode exhaustion independently; heap/cache/orphan/journal exhaustion;
   owner death before/after grant and intent; malformed/replaced/absent records;
   no permission bypass through dup/spawn/PID reuse/credential drop. Disk-space
   progress and safe resource refusal are separate assertions.
4. Cut after every grant/intent/allocation/publication/retirement write and flush,
   including grant update versus allocation in one journal commit, checkpoint
   uncertainty and replay cuts. Recover twice and interrupt recovery again.
   Pre-intent abort leaves old command/state; post-intent valid states roll
   forward under the documented resource assumptions. Never reduce the account
   floor, double charge/refund or expose credit before durable retirement.
5. Private copied-workspace installed NVMe regular-file fixtures only:
   BIOS/UEFI x SMP=1/4, competing allocations before manual recovery, reboot
   twice, retained command/state handles and SSH-loss/signals. Audit Linux
   e2fsck/debugfs bytes, metadata, allocation ownership, floors and unrelated
   files after cuts. Keep source/media preservation distinct from boot results.

Recovery authentication/time policy is also unresolved: blindly applying a newly
expired catalog check to a previously admitted intent may stop roll-forward even
with perfect space accounting. Define how durable authorization is revalidated
without introducing an expired-package install bypass. Space guarantees alone
must not be described as unconditional recovery liveness.

No fresh runtime/boot acceptance or shared-media hash claim follows from this
audit. Source inventory and unchanged-source check are retained in
`.codex-remote-attachments/pkg-recovery-space-review-20261021/manifest.json`.
No build, release-media update, physical write, commit or push was performed.
