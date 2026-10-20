# EH-6 directory rename and subtree relocation

2026-10-20. Implementation contract for the existing no-replace SYS_RENAME.
No new syscall, directory replacement, lock rank, boot ordering or raw-write
admission is introduced. EH-1B remains separately open.

Same-parent rename edits directory entries without changing `..` or parent link
counts. Cross-parent move adds/removes the two entries, decrements/increments
parent link counts and changes the moved directory's `..` inode. The moved
directory retains its inode, generation, children, data and own link count.
Existing rename timestamp behavior is preserved; a consistent mutation
timestamp policy remains the separate EH-1B task.

All checks, planning, commit/checkpoint and cache publication use existing ext4
exclusion. One bounded JBD2 transaction holds every metadata edit, including
directory growth/extent/allocator changes if insertion needs space. Shared inode
table/directory blocks retain all staged edits and receive final checksums.
Parent link overflow/underflow, OOM, path bounds, credits and space reject before
I/O. The existing account allocation floor and fixed-role protections apply.

The destination must be absent (EEXIST otherwise). Follow on-disk destination
ancestry through validated dot entries to reject source/self/descendant cycles
with EINVAL, even when descendants are not cached or the destination child
already exists. Malformed ancestry fails
closed. The bounded traversal rejects excessive depth/work with EFBIG.
Cross-parent directory moves require write permission on the moved directory
as well as existing parent write/search and sticky-directory checks.

Before I/O, reserve a small heap cache-publication plan. Validate all loaded
descendant paths and scan the on-disk subtree to ensure the new prefix does not
make an unloaded child's path exceed VFS_MAX_PATH. Traversal is iterative with
heap scratch, at most 4096 entries/directories and 4096 directory blocks; larger
subtrees reject EFBIG unchanged. No recursion or large stack frames. On success,
rebind the cached root to its new parent and rebase loaded descendant prefixes
without allocation or I/O. Retained descriptor identity, offsets and executable
snapshots survive. New lookups resolve the updated disk/cache namespace.

Uncertain directory-move I/O returns EIO and fences new namespace/open/readdir/
exec admission using namespace-uncertain/recovery-required health until reboot
recovery. Validated retained regular-file reads retain the established policy.
No partial cache move or rollback is claimed. Recovery admits only a consistent
old or new tree with exact allocation ownership. Nonjournaled fallback shares
the planner/publication checks, but retains its existing lack of crash atomicity;
it must not be described as providing JBD2 durability. Actor/Ring 3 mutation
on nonjournaled mounts remains unsupported by the existing admission policy.

Process cwd is currently a stored string, not a retained directory object.
This change rebases VFS node paths, not process cwd strings; an existing cwd
inside the renamed subtree may need an explicit chdir to its new path. No new
cross-rank process/filesystem locking or POSIX inode-based cwd claim is made.

Verification: host ASan/UBSan across 1/2/4 KiB blocks and 512/4096-byte sectors;
same/cross-parent, nested/cold descendants, retained files/directories, cycle,
existing target, path overflow, DAC/sticky/account, allocation/credit and I/O
failures. Enumerate rename write/flush cuts with repeated recovery. Independent
Linux fsck/debugfs checks parent links, `..`, hierarchy and exact payloads.
Release-excluded Ring 3 probe on installed NVMe BIOS/UEFI x SMP=1/4 must pass
shutdown/reboot and offline audits. Kernel/probe frame checks use <=1024 bytes.
Tests run in copied workspaces on disposable regular files. No physical writes,
release build, commit or push are part of this task.
