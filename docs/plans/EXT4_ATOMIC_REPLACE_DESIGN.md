# EH-2: atomic regular-file replacement — implementation review

2026-10-19. **Initial design reviewed; runtime implementation not started.**
This refines [EH-2](EXT4_HARDENING_PLAN.md) from the current implementation.
It does not claim crash acceptance or package-manager readiness.

## Decision and initial scope

Add a dedicated `SYS_RENAME_REPLACE(oldpath, newpath, flags=0)` operation,
with a new number assigned from the ABI at implementation time. Do not reinterpret
unused registers of the existing two-argument `SYS_RENAME` call. Existing rename
continues to return EEXIST on ext4 replacement.

The first replacement operation requires **both names to exist**, referring to
ordinary singly linked regular files on the same healthy journaled ext4 mount.
An absent source or destination returns ENOENT unchanged. New package files use
the existing no-replace rename after staging. This separates initial publication
from replacement, and avoids allocating directory space during replacement.
No directory, symlink, exchange, cross-filesystem, raw E4-A/ext2/runfs fallback,
or multi-file transaction is included. Unsupported backends return EOPNOTSUPP;
there must be no generic unlink-then-rename fallback.

Same path succeeds only after source resolution, regular-file/type, filesystem
and permission admission. Distinct names resolving to the same inode reject as
unsupported until hard links are supported. Trailing slashes reject ENOTDIR.
Unknown flags reject EINVAL. RO/frozen mounts retain EROFS and uncertain/tainted
mounts EIO. Preserve existing path bounds, boot-route protection, immutable
account names/inodes and DAC/sticky-directory policy on both parents/victims.

An explicit credential-aware VFS operation and backend callback are required.
Retain both resolved parent nodes across dispatch. The backend revalidates names,
inode generations, mount identity, state and credentials under the existing
rank-1 `e4_lock`; earlier path resolution alone is not mutation authorization.
No new lock or lock-order change is needed. The syscall copies and validates
both bounded user paths before entering filesystem exclusion. Error ordering
and exact user-pointer cases must be frozen with the ABI implementation tests.

## Source preparation and observable semantics

The caller creates a fresh staging inode, writes its complete bytes, sets final
owner/mode, verifies its digest and closes it before replacement. Initial kernel
admission rejects an open source (EAGAIN), avoiding a surviving writer that can
mutate the newly published inode. This is conservative; relaxing it is later work.
The package transaction must own its staging namespace and serialize operations;
the kernel does not authenticate a package or infer a hash from its filename.

Successful mounted writes already commit/checkpoint synchronously. Replacement
must also complete a device barrier before namespace activation, rejecting a
failed barrier with taint. This establishes data-before-name ordering; it does
not make prior in-place overwrites atomic. Existing holders of the overwritten
destination retain their original inode; ordinary writes through those handles
continue to affect that old inode. New lookup/open sees the source inode with its
final owner/mode. Existing executable snapshots are copied
bytes and remain valid independently of inode reclamation.

The operation consumes the source name on successful replacement. Creation time
and content mtime of the source survive activation. Replacement ctime/parent
timestamp policy must be stated and tested when implemented; do not let a
directory helper's incidental extent rebuild determine that policy.

## One activation transaction

Under existing exclusion, validate the complete directories and both files,
reserve bounded metadata/journal credits, and preflight orphan capacity. All
staging allocation or credit failures precede device writes. In one transaction:

1. Change the existing destination directory entry's inode number to the source.
2. Remove the source directory entry.
3. Set the overwritten destination inode's link count to zero and add it to the
   traditional `s_last_orphan` chain, including the superblock and inode checksums.
4. Update all affected directory checksums and explicitly selected timestamps.

Do not free the victim's data, extent nodes or inode in this transaction. Keep
source link count one; only its name changes. Deduplicate metadata blocks when
both names or inodes share a block, and calculate directory checksums from the
final staged image after all edits.

**Important helper constraint:** `e4_dir_edit()` scans home bytes and rejects an
existing insertion. Calling remove then insert for the destination cannot provide
this operation. Add a bounded replacement editor that changes an existing entry
in the staged image without changing its record length. Same-directory and
same-block source/destination edits must preserve each other's changes. Never
make a second namespace commit to work around this constraint.

Only after activation commit/checkpoint succeeds: register the in-memory orphan,
detach the old cached destination name, mark its node removed, move the cached
source node to the destination, and refresh affected parents, all before releasing
exclusion. Reuse/preflight existing cached nodes; no fallible allocation is
permitted between durable activation and hierarchy publication. Retained victim
nodes must remain alive through existing VFS references and open-file counts.

## Failure outcomes and cleanup

The existing transaction helper returns an error for a failed checkpoint even
when the journal commit may already be durable. Therefore:

| Boundary | Required outcome |
| --- | --- |
| Validation/staging/credit failure | No writes, namespace change or output publication |
| Barrier/log/commit/checkpoint uncertainty | EIO, permanently tainted mount, no rollback or automatic retry claim; recovery decides old/new |
| Successful activation/checkpoint | Publish new namespace and report success |
| Later orphan cleanup cannot proceed | Keep activation successful; retain recoverable cleanup intent; surface cleanup health through sync/freeze |

Never run fallible orphan reclamation before publishing a successful activation,
and never return a cleanup failure as if replacement did not occur. An internal
activation outcome must distinguish a completed replacement from later cleanup.
**Open policy gate before mounted implementation:** current taint rejects
mutations, while `e4_read_locked`, `e4_journal_refresh` and the EXEC permission
path do not universally reject tainted reads. AGENTS.md §7.4 explicitly preserves
read capability after taint. Thus cache invalidation/taint alone does not prove
that a failed multi-directory checkpoint cannot expose a partially checkpointed
namespace. Do not claim this behavior already exists or silently disable all
reads to solve it.

Proposed resolution for review: record namespace uncertainty specifically after
replacement I/O failure, reject new path admission/readdir/exec on that mount
until recovery, and preserve validated retained regular-file descriptor reads.
The activation transaction never changes either file's extent mapping or data.
Retained descriptor handling must still validate inode identity/generation and
cope with a victim whose zero-link intent is not yet registered in memory.
Review this narrower admission contract against existing taint/read guarantees
before changing runtime behavior. An alternative would require a correctly
retained journal overlay for reads, which is substantially larger scope.

Pinned victims survive until final close. Unpinned victims use the existing
bounded right-edge orphan cleanup after activation, with retries via sync/freeze
and restart. A crash after activation but before cleanup leaves a zero-link
orphan and no victim name. Repeated recovery must reclaim it exactly once. Do not
reuse its inode while retained handles still identify it by generation.

## Implementation sequence and release gates

1. Resolve the uncertain-namespace/read-admission policy above. Add the private
   engine activation planner and explicit activation outcome;
   test separate/same directories, shared metadata blocks, unchanged source
   content, zero-length files, short timestamp areas, open-source rejection,
   orphan saturation and exhausted staging credits.
2. Add mounted publication and retention tests with open destination, dup and
   independent positions, concurrent lookup/open/exec/unlink/replace, final-close
   reclamation and cache saturation. Compare file identity as well as bytes.
3. Add the credential-aware VFS and additive syscall, including NULL/unmapped/
   readonly/page-crossing/unterminated/overlength paths and unknown flags. Run
   release-excluded Ring 3 BIOS/UEFI coverage before claiming the ABI delivered.
4. Enumerate every write/flush event in activation and cleanup. Cut before and
   after each event under the existing persistence modes; restart/recover twice,
   check old-or-new destination bytes and metadata, source-name relationship,
   exact allocation ownership and no leaked/aliased/or twice-freed inode/block.
   Include journal wrap, ENOSPC/OOM, 1/2/4 KiB blocks and 512/4096-byte sectors.
5. Independently replay/audit disposable copies with Linux, hash payloads, check
   permissions, run e2fsck and prove source/non-target preservation. Separate
   modeled atomic-sector cuts from sector-tear or physical power-loss evidence.

The old state has both original names and original contents. The new state has
no source name and a destination containing the complete source inode; victim
storage may remain only as a valid orphan. A missing destination, mixed payload,
unexpected name/inode pairing or unowned allocation is a failure.

No release image, physical write, automatic package rollback, host-key rotation,
account database replacement, service restart or boot-layout change is authorized
by delivering this primitive. Package policy must separately protect machine
identity, accounts, base OS and active services.

## Source paths reviewed

- `src/fs/vfs.c:rename_common`, retained parent lookup and backend dispatch.
- `src/fs/ext4_namespace.inc:ext4_engine_namespace_rename`, no-replace engine.
- `src/fs/ext4_write.inc:e4_dir_edit`, home-byte scan and staged directory edits.
- `src/fs/ext4_mount_journal.inc:e4_jrename_account`, permission/state checks and
  hierarchy publication; `e4_jclose`, retained orphan cleanup.
- `src/fs/ext4_orphan.inc:e4_orphan_add`, `e4_orphan_cleanup`, unlink and pins.
- `src/fs/ext4_transaction.inc:e4_transaction_commit`, durable commit versus
  checkpoint, taint and cache publication.
- `src/fs/ext4_permissions.inc:e4_exec_snapshot`, coherent copied image/metadata.
- `src/fs/jbd2_write.inc` and `src/fs/jbd2.h`, ordered data and journal barriers.
