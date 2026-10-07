# EXT2 Write Path Optimization & NVMe Batching — 2026-10-07

## 1. Overview & Baseline

With the introduction of microsecond-resolution TSC timing in \/bin/diskbench\, live NVMe benchmark runs revealed an anomaly in the ext2 write path: write throughput on \/mnt\ was flat at ~1 MiB/s across transfer sizes (16 KiB through 256 KiB), corresponding to a constant latency of ~1.0 ms per KiB. Meanwhile, sequential read throughput was 16–20 MiB/s.

This document records the diagnosis, the architectural fix, before/after benchmarks, and the baseline performance model for ext2 and future ext4 implementations.

## 2. Diagnosis

Analysis of the NVMe driver and filesystem I/O paths identified three bottlenecks:

1. **Synchronous Per-Block Flush Barriers**:
   - In \src/fs/ext2.c\, \ext2_alloc_block()\ executed two synchronous \lock_flush()\ calls for every newly allocated 1024-byte block:
     - Flush barrier 1: after writing the block bitmap sector.
     - Flush barrier 2: after updating the block group descriptor and superblock.
   - For indirect blocks, \ile_block_alloc()\ invoked another synchronous \lock_flush()\.
   - On NVMe in QEMU and bare-metal environments, each \NVME_NVM_OP_FLUSH\ triggers a host durable cache sync (\datasync()\), requiring ~500 µs. Two flushes per 1024-byte block produced the observed ~1.0 ms/KiB write ceiling.
2. **Synchronous Zeroing & Repetitive Bitmap I/O**:
   - Every block allocation zeroed the new block on disk individually and executed synchronous unbuffered sector reads and writes against the block bitmap and group descriptor table.
3. **Single-Sector NVMe Transfers & Polling Overhead**:
   - The NVMe driver originally executed sector I/O strictly one 512-byte sector at a time. Furthermore, vme_submit_io_cmd()\ polled the Controller Fatal Status (\NVME_REG_CSTS\) register on every spin turn, triggering repetitive VM exits.

## 3. The Fix

The write path was overhauled to preserve ext2 crash consistency while batching operations:

1. **Deferred Flush Barriers**:
   - Removed the synchronous per-block flushes from \ext2_alloc_block()\ and \ile_block_alloc()\.
   - Flushes are deferred to the end of the transaction barrier: \write_inode()\ flushes dirty metadata once per write call, and clean unmounts execute a single durable flush barrier.
2. **Block Bitmap In-Memory Caching (\mp_cache\)**:
   - Added an in-memory buffer (\mounted->bmp_cache\) for the active group's block allocation bitmap.
   - Bitmap bit allocations and frees modify the cache in memory and mark it dirty.
   - Dirty bitmap sectors, group descriptors, and superblock counters are flushed once at the end of the write operation.
   - Updated \ext2_validate_block_mapping()\ and \ext_truncate()\ to consult \mp_cache\ so in-flight allocations and deallocations are immediately coherent without stale disk re-reads.
3. **Multi-Sector NVMe I/O & Contiguous Run Coalescing**:
   - Implemented vme_read_sectors()\ and vme_write_sectors()\ in \src/drivers/nvme.c\ and \src/drivers/nvme.h\.
   - Configured \max_run_bytes = 4096\ for \g_nvme_base_dev\ in \src/drivers/block.c\.
   - Updated ext2's byte transfer routines to issue up to 4 KiB (8 sectors) in a single NVMe command.
   - In \ead_inode()\, contiguous block runs are detected and issued in single multi-block requests.
   - Throttled the NVMe \NVME_REG_CSTS\ poll check to once per 1024 spins.

## 4. Benchmark Results

Measurements taken via \disk bench -c\ on QEMU (x86_64, NVMe backing device):

| Transfer Size | Before Throughput | Before Duration | After Throughput | After Duration | Speedup |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **16 KiB** | 1.12 MiB/s | 14.3 ms | **3.31 MiB/s** | **4.7 ms** | **~3.0×** |
| **64 KiB** | 0.99 MiB/s | 64.9 ms | **3.92 MiB/s** | **15.9 ms** | **~4.0×** |
| **128 KiB** | 1.01 MiB/s | 126.5 ms | **3.27 MiB/s** | **38.2 ms** | **~3.3×** |
| **256 KiB** | 0.98 MiB/s | 259.8 ms | **4.29 MiB/s** | **58.2 ms** | **~4.5×** |
| **1 MiB** *(capped at 268 KiB)* | ~1.00 MiB/s | ~268 ms | **4.18 MiB/s** | **62.5 ms** | **~4.3×** |

Sequential read throughput measured between **16.0 MiB/s and 19.7 MiB/s**.

*(Note: Maximum single-file size on 1024-byte block ext2 without double-indirection is 12 direct + 256 indirect = 268 blocks = 274,432 bytes, as documented in ARCH_REVIEW.md).*

## 5. Verification & Testing

- **Phase 9D Persistence Suite (\scripts/test_ext2_write.py\)**: Passed across 3 boots on BIOS and 3 boots on UEFI. Host \e2fsck -fn\ confirmed 0 errors, clean bitmap summaries, and intact directory trees.
- **Disk Bench Integration Suite (\scripts/test_diskbench_qemu.py\)**: Passed all tests (default, \-c\ comparison mode, \-s\ silent summary, and automated tmpfile cleanup).
- **Disk Tool Suite (\scripts/test_disk_qemu.py\, \scripts/test_disk_host.py\)**: All 11 host tests and full QEMU integration tests passed.
