# EH-4 Package-Scale Installed-Root Acceptance Evidence

2026-10-20. **Status: COMPLETE.** Validated 100% across multi-firmware BIOS/UEFI under both SMP=1 and SMP=4 on native installed-root disposable NVMe fixtures (`make test-ext4-pkg-scale`).

## 1. Acceptance Campaign Summary

The EH-4 acceptance campaign validates the complete ext4 hardening stack under realistic package manager operations on a live installed-root system (`boot_mode=installed root=PARTUUID=...`).

Evidence directory: `.codex-remote-attachments/ext4-pkg-scale-20261020-212350/`

| Run | Firmware | SMP | Probe Status | Catalog Entries | Worker PIDs | Linux e2fsck | Linux debugfs Oracle | Overall |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | BIOS | 1 | PASS | 128 / 128 | v1=56, v2=57 | 0 errors | 129 entries, byte-exact match | **PASS** |
| 2 | BIOS | 4 | PASS | 128 / 128 | v1=58, v2=59 | 0 errors | 129 entries, byte-exact match | **PASS** |
| 3 | UEFI | 1 | PASS | 128 / 128 | v1=56, v2=57 | 0 errors | 129 entries, byte-exact match | **PASS** |
| 4 | UEFI | 4 | PASS | 128 / 128 | v1=58, v2=59 | 0 errors | 129 entries, byte-exact match | **PASS** |

## 2. Tested Operations & Invariants

1. **Multi-Block Directory Scaling**:
   - `/opt/pkg/catalog` populated with 128 files, each containing 256 bytes of deterministic payload data.
   - Forced ext4 directory growth across multiple 4 KiB block boundaries.
   - Traversal verified in-guest via `SYS_READDIR` (6) loop, accurately resolving all 128 entries.
   - Post-shutdown host Linux `debugfs` verified 129 directory entries (128 entries + lost+found/dir links) with exact payload SHA-256 digests.

2. **Batched Staging Writes (`SYS_STAGE_WRITE` 85)**:
   - Binaries and payload assets staged using 32-block (128 KiB) synchronous batches.
   - Zero transaction leaks or JBD2 descriptor stalls under SMP=4 multi-core load.

3. **Live Binary Replacement & Open VFS Node Retention**:
   - Worker `v1` spawned from `/opt/pkg/bin/service` and kept running with open file references.
   - File atomically replaced via `SYS_RENAME_REPLACE` (84) with newly staged binary `v2`.
   - Worker `v1` continued uninterrupted execution, writing heartbeats to disk.
   - Worker `v2` subsequently spawned from `/opt/pkg/bin/service`, confirming publication of the new inode.
   - Both worker processes cleanly joined and terminated, verifying deferred orphan list inode reclamation without corruption.

4. **Multi-Core Account Contention Resilience**:
   - `user/tools/sudo.c` enhanced with bounded retry logic (`SYS_NANOSLEEP` 25 ms, 20 attempts) for the atomic account database gate (`accounts.c:enter()`).
   - Sudo elevation succeeded deterministically on SMP=4 across all runs.

5. **Post-Shutdown Host Linux Oracle Verification**:
   - Host `e2fsck -fn` reported 0 defects, 0 free block/inode discrepancies, and clean structural consistency across every run.
   - Host `debugfs` extracted and checked all file contents against reference data.

## 3. Configuration & Artifact Hashes

- Probe binary: `build/pkg-probe.elf` (29,752 bytes).
- Stack frame budget: Measured 336 bytes maximum (Gate G1 limit $\le 512$ bytes).
- Command: `make test-ext4-pkg-scale` (executing `scripts/test_ext4_pkg_scale.py --firmware all --smp all`).
- Serial logs preserved:
  - `serial-bios-smp1.log` (30,839 bytes)
  - `serial-bios-smp4.log` (31,303 bytes)
  - `serial-uefi-smp1.log` (32,381 bytes)
  - `serial-uefi-smp4.log` (32,815 bytes)
  - `result.json` (1,050 bytes)
