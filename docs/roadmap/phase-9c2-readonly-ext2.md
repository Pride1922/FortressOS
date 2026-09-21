# Phase 9C.2 — Read-Only ext2

Status: COMPLETE (superseded/extended by Phase 9D for writable support — see
`phase-9d-writable-ext2.md`). This file was reconstructed from
`ARCH_REVIEW.md`'s "Implemented: Phase 9C.2 read-only ext2" section, which
was its only prior home — it was never in the old monolithic `ROADMAP.md`,
so it fell through the original phase split. It belongs here for
consistency with the other 9-series phase files.

## Implementation

- Mount a preformatted, clean ext2 partition at `/mnt` during boot; root
  USTAR remains accessible. The partition device must live for the entire
  mount.
- Revisions 0/1, 1/2/4 KiB filesystem blocks, 512/4096-byte device sectors,
  power-of-two inode sizes from 128 bytes through filesystem block size.
- Regular files and linear directories; classic direct, single-, double-
  and triple-indirect block lookup. Indirection depth decreases explicitly,
  so repeated pointers cannot cause unbounded recursion. Zero pointers read
  as holes.
- Iterative path lookup, lazy immutable inode snapshots and node caching.
  Limits: 4096 block groups, 1024 cached nodes per mount, 1 MiB per
  directory, 63-byte component names and 255-byte paths. Each read
  transfers at most 64 KiB; larger callers receive a short read and can
  continue.
- Validate superblock geometry/features, table ranges, inode numbers, block
  references and directory record lengths/names. Reject out-of-partition
  reads, dirty filesystems and unsupported formats; mount publication is
  transactional.
- Supported compat bits: `ext_attr`, `resize_inode` and `dir_index`, but
  actual indexed directory inodes are rejected. Supported incompat bit:
  `filetype`. Supported ro_compat bits: `sparse_super` and `large_file`,
  but nonzero high file-size words are rejected. Other feature bits and
  nonzero inode flags are rejected.
- No symlink/device support, journal replay, extents, writes, unmount or
  cache eviction. Unsupported nodes return errors when reached, not a full
  upfront certification of every inode on disk. Cached mount metadata is
  intentionally retained and accounted for; it is not classified as a leak.
- Current polling I/O executes under IRQ masking on one CPU. This bounds
  work but can delay scheduling during disk reads. Sleepable locks,
  asynchronous I/O, device-wide concurrency and mount lifetime management
  precede broader use. (SMP implications: see `SMP_DESIGN.md` Piece 3.)

The fixture generator requires e2fsprogs, creates real file data with
`mke2fs`, and runs `e2fsck`. It no longer substitutes an ext2 signature on
failure or writes raw patterns into filesystem sectors. GPT boundary tests
compare translated partition reads directly with corresponding parent
reads.

## Verification

- `make test-ext2`: actual ext2/VFS sources, host adapters, ASan/UBSan and
  leak detection. 1/2/4 KiB blocks, 512/4096-byte sectors, 128/256-byte
  inodes; malformed geometry/features/table pointers/directory records;
  allocation and I/O failure; sparse reads; indirect pointer bounds and
  fixed-depth traversal.
- `make test-storage`: strict kernel build, BIOS and paired-pflash UEFI
  boot. Includes previous suites, all GPT negative cases, nested
  lookup/enumeration, independent offsets, direct/single/double-indirect
  fixture reads and sparse data. Ring 3 opens `/mnt/hello.txt`, checks
  exact contents, prints, checks EOF, closes, exits, and repeats under the
  allocation-set/mapping/heap audits.
- Host tests exercise triple-indirect traversal with synthetic references;
  a full large on-disk triple-indirect file is not part of the QEMU
  fixture.
- QEMU tests use snapshot disk writes. Logs: `build/storage-bios.log` and
  `build/storage-uefi.log`. Physical Latitude storage was subsequently
  verified via the USB path in Phase 9G.4 (`phase-9g4-usb-durability.md`),
  not this phase — this phase's persistence claim is QEMU-only.

## Related cross-cutting note

GPT partition-entry format support (128/256/512-byte entries, bounded
validation against declared layout, raw-copy backup-GPT handling) is
general and used by this phase, Phase 9G.3, and Phase 9G.4 alike — it's
kept once in `ARCH_REVIEW.md` under "GPT conformance" rather than
duplicated per phase.
