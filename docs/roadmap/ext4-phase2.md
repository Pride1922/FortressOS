# EXT4 Phase 2 — bounded extent and file reads

Implemented 2026-10-03. Read-only admission, lookup/readdir and file reads support the restricted E4-A-v1 profile. Production mount selection and the boot image remain ext2; writes, USB/physical ext4 acceptance and journaling are pending.

`src/fs/ext4.c` validates inline roots and external index/leaf nodes before publishing a file map. External node CRC32C chains the UUID-derived seed, inode number and generation. Headers, depth, capacity, sorted keys, parent ranges, logical overlap, physical bounds, reserved metadata, allocated bitmap bits and duplicate/overlapping physical references are checked. Tree blocks and file extents share the alias check, so a child cycle or a file extent overlapping a tree block fails. Depth is limited to 2; node visits and mount-wide cached extent entries are each capped at 4096. Unsupported deeper or legacy/flagged mappings fail visibly. This validates each accessed inode's tree, not cross-inode ownership or a complete filesystem fsck.

Validated maps and traversal buffers belong to the mount. Maps are cached for immutable RO media and released by failed-mount/test cleanup; production unmount/hot-plug is not introduced. Physical-reference overlap checking is bounded pairwise work during initial map construction. Callbacks hold the existing rank-1 ordinary filesystem lock, use heap-owned working buffers and scalar CRC, and never enable IRQs or schedule during block I/O. Each read returns at most 64 KiB, handles arbitrary offsets/EOF, and zero-fills holes and unwritten extents without reading their data sectors. Data I/O failure returns a completed block prefix when available, otherwise EIO. Ext4 metadata checksums do not checksum regular file contents.

Linux fixture generation uses mke2fs/debugfs with the exact profile and checks every image using `e2fsck -fn`. Each 1/2/4 KiB fixture contains a 1 MiB byte-pattern file, 1,600 isolated extents producing a depth-2 tree, holes, an unwritten preallocation, and a sparse marker beyond logical byte offset 4 GiB. Temporary large sparse host sources live on Linux temporary storage; retained images, manifests, fragmented payloads and tool output are under `build/ext4-phase0`.

Commands actually run:

```text
wsl -d Ubuntu-24.04 -- make
wsl -d Ubuntu-24.04 -- make test-ext4-read-host
wsl -d Ubuntu-24.04 -- make test-ext4-read
wsl -d Ubuntu-24.04 -- make test-ext2
git diff --check
```

Host ASan/UBSan: six block/sector combinations (1/2/4 KiB × 512/4096 bytes). Exact full bytes, depth-2 traversal, sparse offsets above 4 GiB, zero unwritten/hole reads, unaligned block crossing, EOF, 64 KiB cap, completed-prefix I/O failure, OOM/no partial map publication and zero write/flush counters pass. Existing Phase-1 malformed admission and every mount-read failure coverage remains. Hostile tree tests include out-of-range/reserved references, zero lengths, logical/physical overlaps, excessive depth, corrupted external checksums, correctly checksummed child cycles and malformed leaf lengths/keys. Single-threaded lock adapters are not a real IRQ/SMP proof.

QEMU: six BIOS/UEFI × 1/2/4 KiB cases with SMP=1 and a disposable, read-only GPT/NVMe fixture. The known QEMU NVMe PCI identity plus `opt/fortress/ext4_read_test` selects the test path at the existing storage initialization point. Physical hardware exclusion, production USB policy and raw-write gates are preserved. Final argv is checked against the exact allowed command, and extra storage arguments are rejected. UART/argv/stderr/OVMF/fixture evidence is retained; waits are bounded and QEMU is reaped on all exits.

Guest code reads every byte of the 1 MiB, fragmented and unwritten files through the actual VFS and verifies independent deterministic byte patterns. It checks the high-offset sparse marker and EOF. Ring 3 `sha256sum` then independently hashes those three files and the runner compares against Python SHA-256 of the Linux host payloads. Shell prompt recovery is required after every command, and the entire GPT image SHA-256 must remain unchanged. This is actual NVMe/VFS/Ring 3 evidence; it is not USB, physical hardware, SMP filesystem access, crash consistency or write acceptance.

Latest evidence: host fixtures `build/ext4-phase0/fixtures-arxqm1_3/manifest.json`; guest run `build/ext4-read/run-molw9zd6/manifest.json` (6/6), with Linux fixtures `build/ext4-phase0/fixtures-3v3h8b5e/manifest.json`. The initial guest run `build/ext4-read/run-skmtd6x_/manifest.json` also passed 6/6.

Format reference: [Linux ext4 inode/extent layout](https://www.kernel.org/doc/html/latest/filesystems/ext4/ifork.html).

Build and ext2 host regressions pass. Next: Phase 3 allocation and extent mutation engine, with sanitizer/failure-injection gates before any production RW mounting.
