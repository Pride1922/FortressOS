# FortressOS — ext4 Subsystem Support Profile & Technical Specification

**Status:** ACTIVE / SPECIFICATION  
**Date:** 2026-10-25  
**Issue:** [#40](https://github.com/Pride1922/FortressOS/issues/40) (`ext4 support profile: Document supported vs unsupported ext4 feature flags, bounds, and journal contracts`)  
**Scope:** Definitive technical reference of the FortressOS ext4 implementation (`src/fs/ext4.c`, `src/fs/ext4_*.inc`, `src/fs/jbd2.h`). Defines supported vs. unsupported superblock feature flags, geometric and allocation limits, extent tree architectures, orphan list lifecycles, JBD2 journaling contracts, staging batch mutations, and emergency failure taint semantics.

---

## 1. Architectural Philosophy & Scope Boundaries

FortressOS implements an intentional, bounded, secure subset of the ext4 filesystem designed specifically for high-reliability embedded platforms, immutable system volumes, and robust package deployments. It does **not** aim to replicate all historical or experimental features of Linux ext4.

### Core Tenets:
1. **Fail-Closed Admission**: Any volume containing unsupported feature flags, corrupted checksums, unknown directory formats, or unadmitted geometries is immediately rejected with explicit error codes (`-VFS_EOPNOTSUPP` or `-VFS_EIO`). The kernel never performs silent in-place migrations, format conversions, or silent repairs on untrusted media.
2. **Synchronous Durability over Lazy Caching**: All write mutations in production journaled mode (`E4-B`) commit synchronously through JBD2 ordered data mode. When a syscall returns `0`, the operation is durably checkpointed to the underlying storage device and flushes are barrier-acknowledged.
3. **Emergency Taint Protection**: Any uncertain I/O outcome, sector write error, or flush barrier failure immediately transitions the filesystem into a tainted state (`tainted = true`), suppressing further mutations and permanently freezing write access until an offline reboot recovery can inspect the media.
4. **Gate G1 Stack & Heap Conformance**: All ext4 kernel routines operate within the $\le 1024$ B stack limit, using bounded static workspaces or explicit heap blocks under the Rank-1 filesystem lock (`e4_lock`).

---

## 2. Superblock Feature Flag Matrix

The ext4 driver validates the primary superblock at physical offset `1024` (byte offset `0x400` in Block Group 0). Inodes and block groups are admitted during `e4_admit()` (`src/fs/ext4.c:795`).

### 2.1 Compatible Features (`s_feature_compat`, Superblock Offset `0x5C` / `sb + 92`)

| Flag | Value | Status | Kernel Enforcement & Behavior |
|---|---|---|---|
| `EXT4_FEATURE_COMPAT_HAS_JOURNAL` | `0x00000004` | **SUPPORTED** | **Required** for journaled mounts (`ext4_mount_journal_rw`). Must be `0` for non-journaled E4-A read-write mounts. |
| `EXT4_FEATURE_COMPAT_DIR_PREALLOC` | `0x00000001` | **REJECTED** | Legacy directory preallocation; rejected (`-VFS_EOPNOTSUPP`). |
| `EXT4_FEATURE_COMPAT_IMAGIC_INODES` | `0x00000002` | **REJECTED** | AFS imagic inodes; rejected (`-VFS_EOPNOTSUPP`). |
| `EXT4_FEATURE_COMPAT_EXT_ATTR` | `0x00000008` | **REJECTED** | Extended attributes; rejected (`-VFS_EOPNOTSUPP`). |
| `EXT4_FEATURE_COMPAT_RESIZE_INODE` | `0x00000010` | **REJECTED** | Online resizing reserved GDT blocks; rejected (`-VFS_EOPNOTSUPP`). |
| `EXT4_FEATURE_COMPAT_DIR_INDEX` | `0x00000020` | **REJECTED** | HTree directory indexing; rejected (`-VFS_EOPNOTSUPP`). Only standard linear directories are admitted. |
| `EXT4_FEATURE_COMPAT_SPARSE_SUPER2` | `0x00000200` | **REJECTED** | Sparse superblock v2; rejected (`-VFS_EOPNOTSUPP`). |
| `EXT4_FEATURE_COMPAT_FAST_COMMIT` | `0x00000400` | **REJECTED** | JBD2 Fast commits; rejected (`-VFS_EOPNOTSUPP`). Full standard JBD2 transactions only. |

*Exact Admission Mask*:
- Journaled mount (`fs->journal_bootstrap == true`): `s_feature_compat == 0x00000004`.
- Unjournaled mount (`fs->journal_bootstrap == false`): `s_feature_compat == 0x00000000`.

---

### 2.2 Incompatible Features (`s_feature_incompat`, Superblock Offset `0x60` / `sb + 96`)

| Flag | Value | Status | Kernel Enforcement & Behavior |
|---|---|---|---|
| `EXT4_FEATURE_INCOMPAT_FILETYPE` | `0x00000002` | **SUPPORTED** | **Mandatory**. Directory entries record 8-bit `file_type` (`EXT4_FT_*`) matching VFS node types. |
| `EXT4_FEATURE_INCOMPAT_RECOVER` | `0x00000004` | **SUPPORTED** | Permitted during mount replay. Indicates ongoing or uncompleted journal recovery. Marked dirty upon mount activation, cleared upon clean unmount. |
| `EXT4_FEATURE_INCOMPAT_EXTENTS` | `0x00000040` | **SUPPORTED** | **Mandatory**. All files and directories must use extent trees (`i_flags & 0x80000`). Legacy direct/indirect block mappings are strictly rejected. |
| `EXT4_FEATURE_INCOMPAT_COMPRESSION` | `0x00000001` | **REJECTED** | Disk compression; rejected (`-VFS_EOPNOTSUPP`). |
| `EXT4_FEATURE_INCOMPAT_JOURNAL_DEV` | `0x00000008` | **REJECTED** | External journal block device; rejected (`-VFS_EOPNOTSUPP`). Only internal journals (`s_journal_dev == 0`) supported. |
| `EXT4_FEATURE_INCOMPAT_META_BG` | `0x00000010` | **REJECTED** | Meta block groups; rejected (`-VFS_EOPNOTSUPP`). Standard contiguous block group descriptors only. |
| `EXT4_FEATURE_INCOMPAT_64BIT` | `0x00000080` | **REJECTED** | 64-bit block numbers; rejected (`-VFS_EOPNOTSUPP`). Standard 32-bit logical block addresses only. |
| `EXT4_FEATURE_INCOMPAT_MMP` | `0x00000100` | **REJECTED** | Multiple mount protection; rejected (`-VFS_EOPNOTSUPP`). FortressOS enforces strict single-kernel exclusive partition access. |
| `EXT4_FEATURE_INCOMPAT_FLEX_BG` | `0x00000200` | **REJECTED** | Flex block groups; rejected (`-VFS_EOPNOTSUPP`). Bitmaps and inode tables must reside within their respective block group boundaries. |
| `EXT4_FEATURE_INCOMPAT_EA_INODE` | `0x00000400` | **REJECTED** | Large EA inodes; rejected (`-VFS_EOPNOTSUPP`). |
| `EXT4_FEATURE_INCOMPAT_DIRDATA` | `0x00001000` | **REJECTED** | Directory data in dentry; rejected (`-VFS_EOPNOTSUPP`). |
| `EXT4_FEATURE_INCOMPAT_CSUM_SEED` | `0x00002000` | **REJECTED** | Checksum seed in superblock; rejected (`-VFS_EOPNOTSUPP`). Seed is canonically derived from UUID (`s_uuid`). |
| `EXT4_FEATURE_INCOMPAT_LARGEDIR` | `0x00004000` | **REJECTED** | Multi-level 3-level directory index; rejected (`-VFS_EOPNOTSUPP`). |
| `EXT4_FEATURE_INCOMPAT_INLINE_DATA` | `0x00008000` | **REJECTED** | Small data stored in inode body; rejected (`-VFS_EOPNOTSUPP`). |
| `EXT4_FEATURE_INCOMPAT_ENCRYPT` | `0x00010000` | **REJECTED** | Native per-file encryption; rejected (`-VFS_EOPNOTSUPP`). |

*Exact Admission Mask*:
- `s_feature_incompat & ~0x0004 == 0x0042` (`EXT4_FEATURE_INCOMPAT_FILETYPE | EXT4_FEATURE_INCOMPAT_EXTENTS`).

---

### 2.3 Read-Only Compatible Features (`s_feature_ro_compat`, Superblock Offset `0x64` / `sb + 100`)

| Flag | Value | Status | Kernel Enforcement & Behavior |
|---|---|---|---|
| `EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER` | `0x00000001` | **SUPPORTED** | **Mandatory**. Backup superblocks and GDTs exist only in groups 0, 1, and powers of 3, 5, 7. |
| `EXT4_FEATURE_RO_COMPAT_LARGE_FILE` | `0x00000002` | **SUPPORTED** | **Mandatory**. Files $> 2$ GiB permitted. |
| `EXT4_FEATURE_RO_COMPAT_METADATA_CSUM` | `0x00000400` | **SUPPORTED** | **Mandatory**. Full CRC32c validation of superblock, group descriptors, inodes, and extent tree blocks. |
| `EXT4_FEATURE_RO_COMPAT_BTREE_DIR` | `0x00000004` | **REJECTED** | B-tree directory indexing; rejected (`-VFS_EOPNOTSUPP`). |
| `EXT4_FEATURE_RO_COMPAT_HUGE_FILE` | `0x00000008` | **REJECTED** | Sector-unit block counting; rejected (`-VFS_EOPNOTSUPP`). |
| `EXT4_FEATURE_RO_COMPAT_GDT_CSUM` | `0x00000010` | **REJECTED** | Legacy uninit GDT checksums (superseded by `METADATA_CSUM`); rejected (`-VFS_EOPNOTSUPP`). |
| `EXT4_FEATURE_RO_COMPAT_DIR_NLINK` | `0x00000020` | **REJECTED** | $> 65000$ directory subdirectories; rejected (`-VFS_EOPNOTSUPP`). |
| `EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE` | `0x00000040` | **REJECTED** | Dynamic extra isize reservation; rejected (`-VFS_EOPNOTSUPP`). Extra isize must be statically 32 bytes (`s_want_extra_isize == 0`). |
| `EXT4_FEATURE_RO_COMPAT_HAS_SNAPSHOT` | `0x00000080` | **REJECTED** | Snapshot support; rejected (`-VFS_EOPNOTSUPP`). |
| `EXT4_FEATURE_RO_COMPAT_QUOTA` | `0x00000100` | **REJECTED** | Superblock quota accounting; rejected (`-VFS_EOPNOTSUPP`). |
| `EXT4_FEATURE_RO_COMPAT_BIGALLOC` | `0x00000200` | **REJECTED** | Cluster allocation; rejected (`-VFS_EOPNOTSUPP`). One block per allocation cluster strictly required. |
| `EXT4_FEATURE_RO_COMPAT_READONLY` | `0x00001000` | **REJECTED** | Read-only image flag; rejected (`-VFS_EOPNOTSUPP`). |
| `EXT4_FEATURE_RO_COMPAT_PROJECT` | `0x00002000` | **REJECTED** | Project quota accounting; rejected (`-VFS_EOPNOTSUPP`). |
| `EXT4_FEATURE_RO_COMPAT_ORPHAN_FILE` | `0x00004000` | **REJECTED** | Orphan file tracking; rejected (`-VFS_EOPNOTSUPP`). Only standard traditional linked orphan lists supported. |

*Exact Admission Mask*:
- `s_feature_ro_compat == 0x00000403` (`SPARSE_SUPER | LARGE_FILE | METADATA_CSUM`).

---

## 3. Structural Limits & Geometric Bounds

| Parameter | Bound | Source Definition | Architectural Guarantee & Notes |
|---|---|---|---|
| **Max Volume Size** | **1 TiB** ($1,099,511,627,776$ B) | `src/fs/ext4.c:822` | Volumes exceeding 1 TiB return `-VFS_EFBIG`. Protects against 32-bit block integer overflow. |
| **Max Block Groups** | **8,192** | `E4_GROUPS` (`src/fs/ext4.c:825`) | Static descriptor arrays allocated on heap during mount; protects against unbounded kernel allocation. |
| **Max Inode Count** | **1,048,576** | `src/fs/ext4.c:819` | Total inode capacity ceiling. Filesystems with more than 1M inodes fail admission. |
| **Supported Block Sizes** | **1024, 2048, 4096 B** | `src/fs/ext4.c:814` | Superblock log block size shift $\le 2$ (`1024 << shift`). Standard 4096-byte blocks used in production. |
| **Inode Size** | **Statically 256 B** | `s_inode_size == 256` (`src/fs/ext4.c:803`) | Exactly 256-byte on-disk inodes. 128-byte legacy inodes and 512-byte extended inodes rejected. |
| **Inode Extra Size** | **Statically 32 B** | `src/fs/ext4_time.h:37` | Offset 128..159 houses nanosecond timestamp extensions and creation time (`crtime`). |
| **Extent Tree Max Depth** | **$\le 2$** (depth 0, 1, 2) | `src/fs/ext4.h:18` | Inode root extent + up to 1 index level + leaf blocks. Handles files up to multi-gigabytes without deep recursive traversal. |
| **Extent Traversal Budget** | **4,096 entries** | `src/fs/ext4.h:19` | Per-operation loop bound preventing CPU starvation on corrupted or fragmented extent structures. |
| **VFS Node Cache Pool** | **1,024 nodes** | `src/fs/ext4.h:32` | Maximum concurrent active cached VFS nodes. Unpinned nodes reclaimed via LRU eviction. |
| **Metadata Cache Size** | **8 blocks** | `src/fs/ext4.h:29` | Synchronous clean block cache under `e4_lock`. Cache hits retain checksum verification. |
| **Staging Batch Limit** | **32 blocks (128 KiB)** | `EXT4_STAGE_MAX` (`src/fs/ext4.h:46`) | Maximum payload per `SYS_STAGE_WRITE` (Syscall 85) call. Guarantees bounded kernel staging buffer. |
| **Transaction Image Budget**| **64 blocks** | `JBD2_WRITE_IMAGES_MAX` (`src/fs/jbd2.h:52`) | Combined metadata and data blocks permitted in a single atomic transaction. |
| **Max Revoke Records** | **64 revokes** | `src/fs/ext4_engine.h:40` | Block revocation records per commit to prevent replay of reallocated data blocks. |
| **Account Reserve Floor** | **256 KiB / 16 Inodes**| `EXT4_ACCOUNT_RESERVE_*` (`src/fs/ext4.h:94`) | Immutable storage headroom reserved strictly for credential and account modifications. |

---

## 4. Inode Layout & Extent Architecture

### 4.1 Inode Structure Layout (256 Bytes)
Each inode occupies exactly 256 bytes on disk:
- `0x00 - 0x01`: `i_mode` (16-bit type and permissions).
- `0x02 - 0x03`: `i_uid` (low 16 bits of owner UID).
- `0x04 - 0x07`: `i_size_lo` (low 32 bits of byte size).
- `0x08 - 0x0B`: `i_atime` (signed 32-bit access timestamp seconds).
- `0x0C - 0x0F`: `i_ctime` (signed 32-bit inode change timestamp seconds).
- `0x10 - 0x13`: `i_mtime` (signed 32-bit modification timestamp seconds).
- `0x14 - 0x17`: `i_dtime` (deletion timestamp seconds).
- `0x18 - 0x19`: `i_gid` (low 16 bits of group GID).
- `0x1A - 0x1B`: `i_links_count` (hard link counter).
- `0x1C - 0x1F`: `i_blocks_lo` (count of 512-byte blocks allocated).
- `0x20 - 0x23`: `i_flags` (must have `0x80000` `EXT4_EXTENTS_FL` set).
- `0x28 - 0x63`: `i_block[15]` (60 bytes total):
  - `0x28 - 0x33`: Extent Tree Header (`ext4_extent_header`, 12 bytes):
    - `eh_magic`: `0xF30A` (Extent header signature).
    - `eh_entries`: Number of valid extent entries in this node.
    - `eh_max`: Maximum entries ($4$ in inode body, $(bs - 12)/12$ in leaf blocks).
    - `eh_depth`: `0` for leaf node, `1` or `2` for index node.
    - `eh_generation`: Generation counter (`0`).
  - `0x34 - 0x63`: Up to 4 Extent Entries (12 bytes each):
    - *If `eh_depth == 0` (Leaf)*: `ext4_extent` (`ee_block`, `ee_len`, `ee_start_hi`, `ee_start_lo`).
    - *If `eh_depth > 0` (Index)*: `ext4_extent_idx` (`ei_block`, `ei_leaf_lo`, `ei_leaf_hi`, `ei_unused`).
- `0x64 - 0x67`: `i_generation` (file generation / NFS version).
- `0x68 - 0x6B`: `i_file_acl_lo` (extended attribute block, must be 0).
- `0x6C - 0x6F`: `i_size_high` (high 32 bits of 64-bit file size).
- `0x74 - 0x77`: `i_extra_isize` (16 bits, set to `32`), `i_checksum_hi` (16 bits).
- `0x78 - 0x7B`: `i_ctime_extra` (nanoseconds shifted by 2 + 2 epoch bits).
- `0x7C - 0x7F`: `i_mtime_extra` (nanoseconds shifted by 2 + 2 epoch bits).
- `0x80 - 0x83`: `i_atime_extra` (nanoseconds shifted by 2 + 2 epoch bits).
- `0x84 - 0x87`: `i_crtime` (creation timestamp seconds).
- `0x88 - 0x8B`: `i_crtime_extra` (creation nanoseconds + epoch bits).
- `0x84 - 0x87`: `i_projid` (project ID, must be 0).
- `0xFC - 0xFF`: `i_checksum` (CRC32c checksum of UUID + inode number + full 256-byte inode).

### 4.2 Timestamp Range & Encoding (EH-1A Specification)
- Timestamps support dates from epoch 1901 up to year **2446** (`seconds` range `[-2147483648, 15032385535]`).
- Nanoseconds are recorded in the upper 30 bits of the extra field (`nanos << 2`).
- The 2 lowest bits of the extra field encode the epoch epoch overflow (`(seconds - signed_low) / 4294967296LL`).

---

## 5. Traditional Orphan Inode Lifecycle

FortressOS implements the traditional ext4 orphan linked list, anchored by `s_last_orphan` (`sb + 232`) in the primary superblock.

### 5.1 Invariants & Safety Guarantees
1. **Chain Length Cap**: The orphan chain is strictly limited to $\le 64$ entries (`src/fs/ext4.h:135`). Excessively long orphan chains are rejected during mount admission.
2. **Atomic Intent Publication**:
   - When an unlinked file still has open VFS handles (`node->opens > 0`), the inode is prepended to the `s_last_orphan` list in the same transaction that unlinks the directory dentry.
   - When truncating a file, the inode is added to `s_last_orphan` before extents are reclaimed, ensuring that a crash during truncation will resume block reclamation upon next boot.
3. **Deferred Reclamation**: An orphaned inode's physical data blocks and inode table entry are only deallocated when the final handle closes (`node->opens == 0`).
4. **Boot Recovery (`ext4_engine_orphan_recover`)**:
   - During journaled mount (`ext4_mount_journal_rw`), the orphan list is walked.
   - Inodes marked with `i_dtime != 0` (unlinked) have their remaining extent blocks reclaimed and inodes freed.
   - Inodes undergoing truncation have their extents truncated to `i_size` and are removed from the orphan chain.
   - `s_last_orphan` is durably cleared (`0`) in JBD2 before normal filesystem operations commence.

---

## 6. JBD2 Journaling Contracts & Transaction Lifecycle

FortressOS incorporates a native JBD2 engine (`src/fs/jbd2.h`, `src/fs/ext4_journal.inc`, `src/fs/ext4_transaction.inc`) operating in **Ordered Data Mode** (`data=ordered`).

```
                +-------------------------------------------------------+
                |         Ring 3 Syscall (e.g., SYS_STAGE_WRITE)        |
                +-------------------------------------------------------+
                                           |
                                           v
+---------------------------------------------------------------------------------------+
| 1. Stage Data: Write user payload directly to target home blocks (pre-barrier)        |
+---------------------------------------------------------------------------------------+
                                           |
                                           v
+---------------------------------------------------------------------------------------+
| 2. JBD2 Transaction Begin: Reserve ring credits (metadata <= 64, revokes <= 64)       |
+---------------------------------------------------------------------------------------+
                                           |
                                           v
+---------------------------------------------------------------------------------------+
| 3. Stage Metadata: Inode changes, Extent blocks, Group Descriptors, Bitmaps           |
+---------------------------------------------------------------------------------------+
                                           |
                                           v
+---------------------------------------------------------------------------------------+
| 4. Device Flush Barrier 1: Ensure home data payload reaches stable physical media     |
+---------------------------------------------------------------------------------------+
                                           |
                                           v
+---------------------------------------------------------------------------------------+
| 5. JBD2 Commit: Write Descriptor Block, Metadata Images, Commit Block to Journal Ring |
+---------------------------------------------------------------------------------------+
                                           |
                                           v
+---------------------------------------------------------------------------------------+
| 6. Device Flush Barrier 2: Ensure commit block and metadata images are durable        |
+---------------------------------------------------------------------------------------+
                                           |
                                           v
+---------------------------------------------------------------------------------------+
| 7. Checkpoint: Copy committed metadata images from Journal Ring to Home Blocks        |
+---------------------------------------------------------------------------------------+
                                           |
                                           v
+---------------------------------------------------------------------------------------+
| 8. Device Flush Barrier 3: Ensure home metadata blocks are durable                    |
+---------------------------------------------------------------------------------------+
                                           |
                                           v
+---------------------------------------------------------------------------------------+
| 9. Advance Journal Tail: Update JBD2 Superblock to free circular ring buffer space    |
+---------------------------------------------------------------------------------------+
```

### 6.1 JBD2 Feature Flags Supported
- Superblock: V2 superblock (`s_header.h_magic == 0xC03B3998`).
- Supported Incompatible Features:
  - `JBD2_FEATURE_INCOMPAT_REVOKE` (`0x00000001`): Revocation records preventing replay of reallocated data blocks.
  - `JBD2_FEATURE_INCOMPAT_CSUM_V3` (`0x00000002`): Full CRC32c checksumming of all descriptor, revoke, commit, and superblock blocks.
- Rejected Features: `64BIT` (`0x0004`), `ASYNC_COMMIT` (`0x0008`), `FAST_COMMIT` (`0x0010`).

### 6.2 Pre-Flight Read-Only Replay Preview
Before issuing any physical recovery writes during mount, FortressOS constructs a memory-only block preview device (`e4_preview_sector`, `jbd2_preview_read_sector`). The filesystem allocation structures, root inode, and directory trees are verified across the replayed state **before a single recovery sector is touched on disk**.

---

## 7. Mutation Syscalls & Staging Architecture

FortressOS provides explicit, transactional, bounded storage syscalls for package deployment and system configuration:

### 7.1 Atomic File Replacement (`SYS_RENAME_REPLACE`, Syscall 84)
- **Signature**: `sys_rename_replace(const char *oldpath, const char *newpath)`
- **Behavior**: Atomically replaces `newpath` with `oldpath`.
- **Durability Guarantee**:
  - `oldpath` directory entry is removed.
  - `newpath` directory entry is repointed to `oldpath`'s inode.
  - `newpath`'s original inode (the victim) is attached to the orphan list if open handles remain, or reclaimed if unreferenced.
  - All modifications commit in a single atomic JBD2 transaction.
  - Any crash before commit leaves `newpath` completely unchanged; any crash after commit leaves `oldpath` replaced. No intermediate half-replaced state is observable.

### 7.2 Staged Batch Writes (`SYS_STAGE_WRITE`, Syscall 85)
- **Signature**: `sys_stage_write(int fd, const void *buf, size_t count, uint64_t expected_offset)`
- **Behavior**: Writes up to 32 contiguous filesystem blocks (128 KiB, `EXT4_STAGE_MAX`) in a single planned batch.
- **Contract**: Revalidates `expected_offset` against the authoritative on-disk size to protect against concurrent offset advancement. Data blocks are written and metadata updated within an atomic transaction.

### 7.3 Synchronous Flushing (`SYS_FSYNC`, Syscall 86)
- **Signature**: `sys_fsync(int fd)`
- **Behavior**: Synchronously checkpoints all pending transactions for `fd`, forces physical disk flush barriers, and retries any eligible orphan cleanups.

### 7.4 Atomic Directory Relocation
- Supported by the journaled namespace engine.
- Re-parents directory subtrees, atomically updates the `..` entry, adjusts parent link counters, and validates that neither parent nor child paths exceed `VFS_MAX_PATH` or 4,096 directory entries.

---

## 8. Error Handling & The Taint Protocol

The FortressOS ext4 engine implements a strict, irreversible **Taint Protocol**:

1. **Trigger Conditions**:
   - Any sector read or write failure on the underlying block driver.
   - Any flush or barrier acknowledgment failure (`dev->flush() == false`).
   - Superblock or group descriptor checksum corruption detected during transaction staging.
   - Allocation collision or extent header invalidity.
2. **Kernel Response**:
   - `fs->engine->tainted = true;` is latched.
   - The clean metadata cache is purged (`e4_cache_drop(fs, true)`).
   - All subsequent write and truncate syscalls immediately return `-VFS_EIO`.
   - The primary superblock's `s_state` is **never** marked clean (`1`).
   - The filesystem is frozen; read operations on existing valid open handles remain permitted, but namespace modifications and new file creations are forbidden.
   - Clean shutdown unmounting is refused, ensuring Linux `e2fsck` or FortressOS boot recovery will be invoked on subsequent boot.

---

## 9. Verification Matrix & Bare-Metal Evidence

The FortressOS ext4 profile is validated across multiple layers of automated tests and bare-metal acceptance:

| Suite / Target | Scope & Evidence | Result |
|---|---|---|
| **Host Unit Suite** (`make test-ext4-host`) | Full actual-code sanitizer tests covering extent parsing, CRC32c calculation, timestamp bit-packing (EH-1A), and JBD2 replay. | **100% PASS** |
| **Directory Relocation** (`make test-ext4-mv-host`) | 6/6 host tests, 3/3 fallback tests, bounded subtrees, cross-directory move under JBD2. | **100% PASS** |
| **Package-Scale Multi-Firmware** (`make test-ext4-pkg-scale`) | 4/4 configurations (BIOS & UEFI $\times$ SMP=1/4) on native installed-root disposable NVMe fixtures: 32-block staging writes, `SYS_RENAME_REPLACE` (84), 128-file catalog expansion, live binary replacement. | **100% PASS** |
| **Interactive SSH SLA Gate** (`make test-ssh-stage-sla`) | Validated interactive SSH p95 latency remains $\le 55$ ms under continuous 128 KiB staging writes. | **100% PASS** |
| **Dell Latitude 5590 Physical Bare-Metal** | Verified on Dell Latitude 5590 with SanDisk 4 GB USB storage: 1 MiB write in 6–7s, 16 MiB write in 1m52s, clean reboot persistence, zero corruption, post-shutdown Linux Mint `e2fsck -fn` 0 errors across 5 consecutive passes. | **USER CONFIRMED PASS** |
| **Linux Oracle Cross-Audit** | Every disk image mutated by FortressOS in test suites is audited with Linux `e2fsck -fn` (0 errors) and `debugfs` SHA-256 byte-for-byte verification. | **100% PASS** |
