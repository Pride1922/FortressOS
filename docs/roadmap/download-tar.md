# Download tools milestone 2 — safe USTAR tar extraction

2026-10-03. `/bin/tar` is implemented and packaged in the normal initramfs. It leaves kernel, ext2, VFS, and network ABIs unchanged. This userspace tool implements uncompressed POSIX USTAR listing (`-tf`) and extraction (`-xf ARCHIVE -C NEW_DIR`).

## Changed-archive policy decision (Option A)

The tar design is unblocked with Option A:
- **Partial destination retained and reported**: If extraction fails partway (including I/O write failures or source change during extraction), the tool reports the retained directory path and exits nonzero. It does not recursively delete the tree, because another process may have modified it.
- **Changed-archive abort before creating destination**: If the SHA-256 recorded during validation pass 1 differs from the hash observed during validation pass 2, the extraction aborts before creating the destination. Partial extraction under a changed archive observed prior to extraction is not permitted. The two-pass validation is the security boundary; the destination is only created after both passes agree.

## Caps, Bounds, and Rejections

- **Supported types**: Uncompressed POSIX USTAR regular files and directories only.
- **Rejections**: Rejects symlinks ('2'), hard links ('1'), devices ('3', '4'), FIFOs ('6'), sparse files, PAX/GNU extensions ('g', 'x', 'L', 'K'), and compressed archives (gzip, bzip2, xz).
- **Paths**: Rejects absolute paths, parent directory traversal (`..` or `.`), empty components, duplicate member paths, and file/directory prefix conflicts.
- **Destination**: Rejects an existing destination with a clear error and nonzero exit.
- **Caps**: Maximum 256 members, depth 16, 16 MiB per file, 64 MiB total expanded data, 4096-byte streaming buffer. Tables are kept off the small userspace stack in BSS.
- **End markers**: Requires two 512-byte zero end blocks and only zero padding thereafter.
- **Bounds**: Full path < 256 bytes, each component < 64 bytes.

## Executed Gates

1. **Host unit and integration suite under ASan and UBSan (`make test-tar-host` / `python3 scripts/test_tar_host.py`)**:
   - Usage, flags, and option validation (`--help`, invalid flags, conflicting options, missing operands).
   - Valid USTAR listing (`-tf`) and byte-exact extraction (`-xf -C`) across files, directories, and nested hierarchies.
   - Reject extraction into existing destination with clear error and nonzero exit.
   - Option A changed archive verification: tampering before pass 2 aborts before destination creation; destination directory does not exist.
   - Option A partial extraction verification: write failure partway through extraction retains partial destination, reports retained path, and exits nonzero.
   - Full rejection matrix: symlinks, hardlinks, character/block devices, FIFOs, PAX/GNU extensions, compressed formats (gzip), absolute paths, path traversal, depth > 16, component >= 64 bytes, file size > 16 MiB, duplicate member paths, file/directory prefix conflicts, truncated archives, and non-zero trailing padding.
   - All tests pass with zero leaks and clean address/undefined sanitizer reports.

2. **QEMU live ext2 integration (`make test-tar` / `python3 scripts/test_tar.py`)**:
   - Boots QEMU with Ring 3 shell, updated initramfs, and disposable NVMe ext2 fixture.
   - Lists archive members with `tar -tf`.
   - Extracts USTAR archive into fresh directory `/mnt/extracted` on ext2 partition with `tar -xf`.
   - Verifies exact contents of extracted files (`file1.txt`, `nested.txt`) and directory structure.
   - Verifies rejection of extraction into existing `/mnt/extracted`.
   - Verifies rejection of symlink, path traversal, and corrupted archives without creating target directories.
   - Flushes with `sync` and performs clean ACPI S5 shutdown (QEMU exit code 0).
   - Runs offline host `e2fsck -fn` on persisted ext2 partition: 0 filesystem errors reported.

3. **Regressions**:
   - `make test-checksum-host`: PASS
   - `make test-stream-tools-host`: PASS

## Physical Acceptance Protocol (Dell Latitude 5590)

The physical acceptance gate combines wget, sha256sum, and tar on real hardware:
1. Boot FortressOS on Dell 5590 with production USB storage mounted at `/mnt`.
2. Download USTAR archive and SHA-256 manifest from LAN HTTP server:
   ```sh
   wget -O /mnt/archive.tar http://<server>/archive.tar
   wget -O /mnt/archive.sha256 http://<server>/archive.sha256
   ```
3. Verify integrity:
   ```sh
   cd /mnt
   sha256sum -c archive.sha256
   ```
4. Extract into fresh directory:
   ```sh
   tar -xf /mnt/archive.tar -C /mnt/extracted
   ```
5. Verify extracted contents:
   ```sh
   ls /mnt/extracted
   cat /mnt/extracted/sample.txt
   ```
6. Sync and orderly shutdown:
   ```sh
   sync
   shutdown
   ```
7. Verify offline filesystem on second host with `e2fsck -fn <partition>`.
