# Package-Scale Installed-Root Acceptance — Design and Architecture (EH-4)

2026-10-20. **Status: COMPLETE.** Validated 100% across multi-firmware BIOS/UEFI under both SMP=1 and SMP=4 on native installed-root disposable NVMe fixtures (`make test-ext4-pkg-scale`).

## 1. Objective and Scope

Phase EH-4 exercises FortressOS's ext4 filesystem implementation under a realistic, dense package-scale workload on a native installed-root volume (`boot_mode=installed root=PARTUUID=...`). 

Prior hardening phases verified individual primitives in isolation:
- **EH-1A**: Inode creation-offset bug fix and extra inode timestamp encoding (`docs/plans/EXT4_INODE_TIMESTAMPS_DESIGN.md`).
- **EH-2**: Atomic file replacement via `SYS_RENAME_REPLACE` (84) with deferred orphan reclamation (`docs/plans/EXT4_ATOMIC_REPLACE_DESIGN.md`).
- **EH-3**: Durability, health tracking, and reserved space floors (`docs/plans/EXT4_DURABILITY_HEALTH_DESIGN.md`).
- **EH-5**: Bounded synchronous staging batches (`SYS_STAGE_WRITE` 85, 32 blocks / 128 KiB ceiling) and scoped mount flush (`SYS_FSYNC` 86) with an interactive SSH SLA gate (`docs/plans/EXT4_STAGING_BATCH_DESIGN.md`).

EH-4 synthesizes these capabilities into an end-to-end package manager installation simulation on live installed-root media, verifying that dense updates, directory growth across multiple blocks, live executable replacement, and background concurrency leave the filesystem structurally sound and byte-exact across reboots.

## 2. Workload Specifications and Constraints

### 2.1 File Tree Staging & Directory Scaling
Package installation packages contain numerous metadata entries, libraries, and binaries. 
- **Catalog Directory Growth**: A single 4 KiB ext4 block holds ~60–80 directory entries depending on filename length. EH-4 creates 128 distinct package catalog entries (`/opt/pkg/catalog/pkg_entry_XXXX.dat`) to force directory allocation beyond a single block, creating additional index/directory leaf blocks and asserting that `SYS_READDIR` (6) correctly traverses directory block boundaries without truncated or duplicated entries.
- **Batched Staging Writes**: Large binaries (e.g. `service_v1`, `service_v2`, and payload assets) are written using `SYS_STAGE_WRITE` (85) with 32-block (128 KiB) chunks, ensuring that the JBD2 transaction batching mechanism operates safely on the root filesystem under multi-core concurrency.

### 2.2 Live Running Binary Replacement (Open Inode Retention)
Real-world package upgrades must replace active service binaries without corrupting running processes or crashing the supervisor:
1. Stage new executable `/opt/pkg/bin/service.tmp`.
2. Spawn worker process `v1` executing the existing binary `/opt/pkg/bin/service`.
3. Worker `v1` opens the executable descriptor and periodically writes heartbeat tokens to `/opt/pkg/heartbeat.log`.
4. Replace active binary atomically via `SYS_RENAME_REPLACE("/opt/pkg/bin/service.tmp", "/opt/pkg/bin/service")`.
5. Verify that Worker `v1` continues running undisturbed via its pinned open VFS node / ext4 inode reference, even though the directory entry now points to the new inode.
6. Spawn worker process `v2` from `/opt/pkg/bin/service` and verify it executes the new `v2` payload with distinct PID and identity.
7. Gracefully terminate worker `v1` and worker `v2`, verifying that ext4 orphan list processing releases the unlinked `v1` inode without leakage or filesystem corruption.

### 2.3 Concurrent Reader & Mutation Contention
While package updates and directory creations take place:
- Concurrent background tasks perform read sweeps across existing files (`/opt/pkg/manifest.txt`).
- Multiple cores (SMP=4) contend on the shared `ext4_engine` and JBD2 writer mutexes, ensuring no deadlocks, transaction leaks, or torn reads occur.

### 2.4 Durability and Oracle Verification
- Following package staging and replacement, `SYS_SYNC` (14) flushes all dirty metadata, unlinked orphan entries, and directory blocks to disk.
- System executes clean shutdown (`SYS_REBOOT` with freeze/poweroff).
- Host Linux environment mounts the extracted root partition read-only and runs:
  1. `e2fsck -fn`: Enforces strict zero-defect structural consistency.
  2. `debugfs`: Enumerates all 128+ directory entries, inspects inode modes and extents, and extracts file contents to verify SHA-256 byte-level identity against the expected payload oracle.

## 3. Architecture & Test Harness (`tests/pkg_probe.c` & `scripts/test_ext4_pkg_scale.py`)

### 3.1 Kernel & System Integration
- **Probe Binary**: Freestanding C binary compiled with `-mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone -mgeneral-regs-only` and `-Wframe-larger-than=512`. Measured maximum stack frame is 336 bytes, strictly honoring Gate G1. Zero dynamic allocations.
- **Direct Syscalls**:
  - `SYS_STAGE_WRITE` (85): 32-block synchronous staging batches.
  - `SYS_RENAME_REPLACE` (84): Atomic replacement.
  - `SYS_READDIR` (6): Multi-block directory entry traversal.
  - `SYS_LIST_DIR` (82): Fixed 16-entry discovery check.
  - `SYS_SPAWN` (17): Process creation for worker binaries.
  - `SYS_SYNC` (14): Filesystem barrier flush.
- **Multi-Core Account Contention Guard**: `user/tools/sudo.c` enhanced with bounded retry logic (`SYS_NANOSLEEP` 25 ms, 20 attempts) for the atomic account database gate (`accounts.c:enter()`), preventing transient `EAGAIN` races under SMP=4.

### 3.2 Acceptance Matrix
| Target Config | Firmware | SMP Cores | Disk Target | Validation Gate |
|---|---|---|---|---|
| config-1 | BIOS | 1 | NVMe 384 MiB | Installed boot, probe PASS, e2fsck 0 err, debugfs match |
| config-2 | BIOS | 4 | NVMe 384 MiB | Installed boot, probe PASS, e2fsck 0 err, debugfs match |
| config-3 | UEFI | 1 | NVMe 384 MiB | Installed boot, probe PASS, e2fsck 0 err, debugfs match |
| config-4 | UEFI | 4 | NVMe 384 MiB | Installed boot, probe PASS, e2fsck 0 err, debugfs match |
