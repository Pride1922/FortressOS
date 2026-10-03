# EXT4 Phase 4 - non-journaled VFS writes and namespace

Implemented and verified: 2026-10-03. Host 6/6 and BIOS/UEFI 6/6 configurations (18 boots) PASS. Production dispatch and the boot image remain ext2. No hardware write, USB ext4, journaling or crash-recovery acceptance is claimed.

## Implementation and supported operations

`src/fs/ext4_write.inc` shares the actual reader and bounded mutation engine. Explicit `ext4_mount_rw` validates the frozen E4-A profile and allocates its engine and mapping workspace before publication. Admission writes and flushes nothing; caller still owns external partition/GPT/durability eligibility. The in-tree RW caller is restricted to the explicitly keyed QEMU fixture, additionally behind the existing QEMU NVMe PCI identity gate. Physical NVMe exclusion and USB selection/durability policy are unchanged.

Delivered: reads, byte writes/overwrite, serialized append, immediate allocation, sparse EOF gaps, partial unwritten conversion, create/mkdir, checked linear directory growth, unlink/rmdir with block/inode reclamation, regular-file same-filesystem rename without replacement, truncate-to-zero, mid-session sync, and shutdown freeze/clean marking. Initialized allocations become durable before references; references become durable before allocation release. Directory, inode, extent, bitmap, group and superblock checksums/counters are maintained in the same bounded plan.

All byte-write planning and commit execute under the existing rank-1 ext4 lock, with preallocated buffers and synchronous block I/O. No new sleep, IRQ enable, worker, lock rank or DMA ownership path was added. The workspace contains 64 distinct filesystem block images including staged data; callbacks return at most 32 KiB written (64 KiB read). Overflow, 8 GiB file limits and credit exhaustion reject before the first write. Large initialized gaps and highly fragmented deletion can exceed credits and return EFBIG; there is no partial namespace or split publication on a planning failure. Public workbench wrappers and VFS callbacks use shared unlocked engine helpers to avoid recursive locking.

RO keeps immutable mappings; RW uses one fixed validated map keyed by inode/generation, reused for repeated reads and invalidated after every successful or uncertain mutation. The first QEMU hashing attempt exposed the cost of rebuilding the same map on every read; cache invalidation remains exercised by overwrites, growth, truncate, unlink/reuse and alternating files. This is a map cache, not delayed writeback.

No wall-clock source exists in the current kernel. Existing inode timestamp fields are preserved; new inodes have zero timestamps. This phase does not invent current Unix times or change ext2's timestamp behavior. A future wall-clock API can supply timestamp updates separately.

## VFS lifetime and namespace decisions

`vfs_node_t.open` pins each independent file_t and pairs with its final close; dup changes only the file_t reference count. EXT4 open counts and deletion checks share the filesystem lock. Active-target deletion returns EOPNOTSUPP, with no disk change. Cached nodes are owned by the mount and retained as removed tombstones, so a lookup-to-open race cannot dereference freed storage or silently open a reused inode. There is a cumulative 1024-node mount-lifetime cap, including tombstones.

Filesystem-owned unlink/rename callbacks update their cached hierarchy before unlocking; VFS does not repeat the detach/free/move. The managed callback also owns authoritative size updates, so VFS does not overwrite a concurrent truncate with an old write result. Existing ext2, pipe and tar nodes leave these optional fields zero and retain their existing behavior.

`rename_no_replace` is checked before VFS's historical destination-unlink step: EEXIST preserves both names, including an open destination. Regular-file moves within or across directories are supported. Directory rename returns EOPNOTSUPP; descendant path and parent-link changes are deferred. Only truncate-to-zero is supported. Existing multiple-link files and unsupported inode features reject mutation. Empty-directory removal validates unique dot/dotdot identities/types and link counts before reclamation.

## Failures and persistence boundaries

Commit, write or barrier uncertainty permanently taints the mount. Further mutation, sync and clean-success claims return EIO without new writes. Reads remain available where their validated backing state permits them. Successfully completed prior callbacks form the returned short-write prefix; the failing callback makes no rollback claim. Ordinary sync flushes without freezing or marking clean. Freeze excludes new mutations and only a healthy completed barrier/clean-state sequence returns success.

Dirty/recovery-needed admission performs no repair or write. Host tests discard runtime state after a dirty mutation and verify RW remount rejects. E4-A crash cuts can leave leaked allocations, partial namespace changes or old/new overwritten data. Fault tests cover these failure boundaries, not journaling guarantees; volatile-cache/torn-sector/replay campaigns remain Phases 6-9. A failed clean write/barrier may have reached the medium, but the current mount never claims clean success afterward.

## Tests and evidence

Host `make test-ext4-write-host` uses actual ext4 and actual VFS with pthread exclusion and memory-sector adapters under ASan/UBSan. It creates unique disposable regular images, never device paths; final output files use exclusive creation. Six 1/2/4 KiB x 512/4096-sector combinations cover:

- Exact 1 MiB and 16 MiB writes/readback, overwrite, EOF-gap zeros, sparse byte writes above 4 GiB, and partial unwritten conversion over media deliberately filled with stale 0xA5 bytes.
- Interleaved physical allocation forcing fragmented roots, external leaves and depth-2 trees, growth/merge, truncate/reclamation, directory record splitting/growth, and inode group changes.
- Four pthread appenders, 400 unique intact records, independent opens/dup lifetime, active deletion rejection, regular cross-directory rename, replacement preservation, directory-rename rejection, empty/nonempty rmdir, freeze and fresh RO remount.
- Every RW admission read/allocation failure; every read, sector-write and flush event for representative create, mkdir, unlink, rename, truncate, gap growth and overwrite; accepted-write/error-return uncertainty; every final-close failure and mid-session sync failure. No publication/leak on admission OOM, no writes on planning denial, permanent post-commit taint, and no fallback heap allocation in byte-write callbacks.
- Independent Linux e2fsck -fn on orderly frozen images and debugfs byte-for-byte 16 MiB dumps. No repair run is used as proof of correctness.

`make test-ext4-write` creates disposable GPT/NVMe images and an isolated ISO, preflights exact argv on each boot, uses paired OVMF, bounds waits and always reaps QEMU. BIOS/UEFI x 1/2/4 KiB cases each use three boots. Boot 1 saves/reopens a 16 MiB file through real kernel/VFS callbacks, exercises namespace operations and downloads both 1 MiB and 16 MiB from an independent host HTTP server with real Ring 3 wget. Every boot runs guest SHA-256 and exact kernel byte checks; boot 2 deletes namespace entries and boot 3 verifies their absence. Public sync/freeze is exercised through an explicitly fixture-only control node. Every orderly stop checks primary s_state == 1, runs offline Linux fsck and compares all three Linux dumps exactly. No physical media or USB ext4 is attached.

The fixture control initially expected echo's word and newline in one write; the shell sends them separately. Dirty remount rejection exposed this test bug. The final control accepts both forms, the runner requires explicit SYNC/FREEZE PASS markers, and the offline audit checks clean state independently of fsck's exit code. Failed/superseded unique runs are retained and are not acceptance evidence.

The QEMU fixture records the maximum complete 32 KiB write callback in raw TSC cycles. These measurements include synchronous device I/O and do not establish physical latency or a calibrated wall-time bound. Device/IRQ latency remains a Phase-5 physical gate.

Shared VFS regressions run separately: ext2 host and BIOS/UEFI three-boot persistence, ext2 SMP=4 append, storage boot audits, pipe/SIGPIPE host, USB host durability/selection, USB read-only mounts and ext2 three-boot USB persistence. Older runners were updated for the cwd prompt, explicit RO fixture selection (the current image's first entry is RW), the shipped AZERTY keyboard versus US QMP keycodes, and current cat failure text. Production boot entries, keyboard default and USB policy were not changed.

Final evidence:

| Command | Result / artifact |
| --- | --- |
| `make` | Strict build PASS, no new compiler warnings; production image remains ext2 and its filesystem verification passes. |
| `make test-ext4-write-host` | 6/6 ASan/UBSan, Linux fsck and exact dumps PASS; `build/ext4-phase4/host-vuucjznj/manifest.json`. |
| `python3 scripts/test_ext4_write.py` | 6/6 BIOS/UEFI configurations, 18/18 boots PASS; `build/ext4-write/run-2dsiu4v4/manifest.json`. Uses the same runner as `make test-ext4-write`, after the strict build. |
| `make test-ext4-alloc-host` | 12/12 PASS; `build/ext4-phase3/run-eduo094z/manifest.json`. |
| `make test-ext4-read-host` / `python3 scripts/test_ext4_read.py` | Host six geometries and QEMU 6/6 PASS; final QEMU `build/ext4-read/run-wh2p4hpi/manifest.json`. |
| `make test-ext2` | Eight host geometries PASS. |
| `make test-ext2-write test-smp-append` | BIOS/UEFI three-boot persistence and SMP=4 independent/shared append PASS; offline fsck clean. |
| `make test-storage` | BIOS/UEFI storage audits PASS. |
| `make test-pipe-host` | Pipe and SIGPIPE sanitizer regressions PASS. |
| USB host durability/mount targets | Host BOT and mount eligibility checks PASS through `make test-usb-persistence` and `make test-usb-mount`. |
| `python3 scripts/test_usb_persistence.py` | BIOS/UEFI explicit RO plus three-boot ext2 RW persistence PASS, offline fsck clean. |
| `python3 scripts/test_usb_discovery.py --mount` | BIOS/UEFI controller present/absent: 4/4 PASS after runner compatibility fixes. |

Some earlier aggregate USB invocations failed on stale runner assumptions; the corrected final runner commands above passed. The documentation-only target name `test-xhci-bot-host` does not exist in the Makefile; the actual host checks were run through the existing USB targets.

Format references: [Linux linear directory/checksum tails](https://www.kernel.org/doc/html/latest/filesystems/ext4/directory.html), [Linux inode layout](https://www.kernel.org/doc/html/latest/filesystems/ext4/inodes.html).

Phase 5 handoff: feature-based production USB mount dispatch, SYS_SYNC/shutdown dispatch, separately labelled opt-in image and physical/SMP/durability acceptance. Current Dell downloads still use ext2 until that integration is implemented; this phase alone does not switch the physical data filesystem.
