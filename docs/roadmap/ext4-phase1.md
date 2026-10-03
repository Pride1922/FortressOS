# EXT4 Phase 1 — read-only metadata

Implemented and host-verified 2026-10-03. Runtime production mount dispatch, file content reads, external extent trees, writes and journaling remain pending.

Implementation: `src/fs/ext4.c` / `ext4.h`. Only the exact E4-A-v1 feature masks are admitted. Mount checks superblock CRC32C, clean/recovery state, filesystem/sector geometry, bounds, group count and counters, group descriptor CRCs, nonoverlapping reserved metadata, initialized bitmap CRCs, root inode CRC and allocated identity, and linear root-directory checksums/records/dot identities before publishing a detached root node at `/mnt`.

Read-only callbacks provide bounded lookup/readdir and metadata sizes with checksummed inode reads. Mount and callback working buffers are heap-owned; scalar CRC32C uses no SIMD. Filesystem blocks 1/2/4 KiB work against single-threaded fake sector devices of 512/4096 bytes. RW always returns EROFS, file reads return EOPNOTSUPP until Phase 2, and sync/freeze of the RO mount perform no storage write or flush. The driver is compiled into the kernel but is not called from production USB mount selection. Ext2 remains the image and production filesystem.

Directory mapping in this phase is deliberately restricted to up to four initialized inline extents, with bounded logical/physical ranges and reserved-metadata exclusion. External directory trees reject with EOPNOTSUPP. Full file mapping, holes/unwritten extents and external trees are Phase 2, as planned.

Linux mkfs with eager inode-table initialization can retain logical INODE_UNINIT/BLOCK_UNINIT bitmap flags on unused groups. The RO driver validates these flags/descriptors and reserves their metadata, skips undefined bitmap checksums, and refuses accessible inode/directory-data lookup through an uninitialized bitmap. This is distinct from accepting lazy allocation/writing. Mutation-side initialization is future work. Mount does not claim to run a complete fsck over every allocated inode or directory subtree.

Commands actually run:

```text
wsl -d Ubuntu-24.04 -- make test-ext4-format-host
wsl -d Ubuntu-24.04 -- make test-ext2
wsl -d Ubuntu-24.04 -- make
git diff --check
```

Results: ext4 ASan/UBSan PASS in all six block/sector combinations; existing ext2 host tests PASS; strict freestanding build PASS. Host locking adapter asserts no recursive/nested acquisition but does not prove real IRQ/SMP behavior. Backend write/flush counters remain zero on successful mounts, callbacks, sync/freeze, rejected inputs and allocation/I/O failures. Rejected mounts publish no VFS node, clear the output pointer and leak no allocations.

Coverage: independent Linux-generated superblock/group/bitmap/inode/directory checksum vectors plus the standard CRC32C known-answer vector; lookup and listing of the 1 MiB file's metadata (not content); repeated lookup; lookup OOM; each of four mount allocation failures; failure at every successful mount read boundary; valid-checksum unsupported feature masks/RECOVER; dirty state; truncated device; invalid geometry; descriptor/checksum corruption; valid-checksum bitmap overlap; bitmap/inode/directory corruptions; correctly checksummed zero-length directory records; bad checksum-tail shape; unsupported indexed and external-tree directories. These distinguish structural rejection from checksum-only rejection.

Latest Linux fixture evidence: `build/ext4-phase0/fixtures-de349cac/manifest.json`; each generated image was independently checked using debugfs/dumpe2fs/e2fsck before driver tests. Host fixtures and images are disposable regular files. No QEMU, USB transport, physical ext4 mount, file-content acceptance, crash recovery or journaling pass is claimed.

Public VFS lookup returns a pointer and cannot convey a distinct corruption/unsupported errno; lookup safely returns NULL. Readdir and mount report negative errno. Improving generic lookup error propagation is separate work. Node count is bounded to 1024; existing VFS path/name limits apply. No production unmount API or hot-plug lifetime is introduced.

Next: Phase 2 extent/file reads and independent byte/digest checks, followed by its BIOS/UEFI guest fixture runner. Do not switch the boot image or selected production USB partition format based on this host gate alone.
