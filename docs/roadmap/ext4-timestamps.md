# EXT4 EH-1A timestamp work — 2026-10-19

Status: **EH-1A COMPLETE — HOST SCOPE**. The hardening plan is
committed as `e34e628`; TLS/HTTPS delivery remains separate.

## Defect and change

The previous inode creation path stored epoch seconds at offset 148 (creation
time extension) rather than offset 144 (creation seconds). The mounted actual-code
test reproduced zero creation seconds with epoch seconds in the extension before
the fix. Baseline diagnostic: `build/ext4-time/baseline-failure.json` (local,
ignored evidence): `expected 1792368000, seconds=0 extra=1792368000`.

The codec now writes signed low seconds, two epoch bits and nanoseconds at named
offsets, respecting the inode's declared extra area. Creation initializes atime,
ctime, mtime and crtime. Existing rebuild/write timestamp updates replace both
ctime/mtime seconds and extensions, clearing inherited fractional and epoch bits.
The kernel clock and timestamp policy outside these existing update paths are
unchanged. Missing extension rejects out-of-range seconds before publication.

The 1 KiB fixture exposed a second defect: the orphan ownership audit compared
an absolute block bitmap with a group-relative bitmap as if both started on a
byte boundary. With `s_first_data_block=1`, its first bit was wrong. The audit
now shifts the absolute bitmap to the group's starting block, retaining exact
allocation equality and free-count checks. The old code rejected the Linux-clean
fixture with EIO at `recovery-preview`, before timestamp mutation.

## Verified

`make test-ext4-time-host` passes actual mounted VFS/EXT4/JBD2 code under ASan
and UBSan. Six profiles: filesystem blocks 1024/2048/4096 bytes crossed with device
sectors 512/4096 bytes. Each exercises epochs 1792368000, 2147483647,
2147483648 and 4102444800, file creation, directory creation, stale extension
replacement on write and unchanged creation time. Twenty-four epoch filesystems
pass independent Linux `e2fsck -fn`, `debugfs` atime/ctime/mtime/crtime decoding,
owner/mode and exact payload checks. Source fixtures retain their original hashes.

Thirty additional mounted cases cover declared inode extra sizes 4/8/12/16/20.
Representable overwrite/append succeeds without changing bytes beyond the declared
area. Unrepresentable writes/append preserve authoritative inode bytes, descriptor
positions, file size/content and complete stable/volatile disk buffers; device
write and flush counters do not advance. Out-of-range creation and mkdir fail
without publishing names or changing the disk. All thirty resulting filesystems
also pass independent Linux fsck and exact payload checks (54 audited images total).
Extra sizes 12/16/20 additionally accept mounted year-2100 writes while preserving
all bytes beyond the declared area.

Six negative ownership cases deliberately allocate a block without an inode owner,
while preserving bitmap checksums and free counters. Every geometry rejects at
recovery preview before writes, flushes or publication, with unchanged disk bytes.

Pure codec cases additionally cover negative seconds, minimum/maximum encoding,
nanosecond bounds, invalid values preserving outputs, and short declared extra
areas preserving untouched bytes. A separate strong test clock overrides the
existing weak host clock; production clock plumbing has no test hook.

Local evidence: `build/ext4-time/manifest.json` (source/binary hashes and profiles)
and per-profile host/Linux logs.
Disposable image files are removed by the runner. No secret material is involved.

Additional regression: `make test-ext4-mount-host` PASS **12/12** across all six
geometries with normal/wrapped journals. It covers 16,480 modeled interruption
cuts, staging read/allocation failures, invalid replayed ownership, retained
open/unlink lifetime, sync/freeze, taint and pthread write/freeze races, followed
by 72 independent Linux replay/fsck/namespace/payload audits. Retained disposable
fixture evidence: `.codex-remote-attachments/ext4-phase8-5/host-gdlt_qb8/manifest.json`
and adjacent logs. These fixtures contain synthetic files, no private keys.

Freestanding `make build/fs/ext4.o` and `git diff --check` also pass. No syscall
or firmware-specific timestamp path was introduced; host coverage uses the
actual mounted mutation code rather than a separate model.

## Remaining scope

Broader chmod/chown/rename/truncate timestamp policy belongs to EH-1B. EH-2 has
a separate [atomic replacement review](../plans/EXT4_ATOMIC_REPLACE_DESIGN.md);
replacement runtime implementation is not included here.

No firmware, physical-device, crash durability, atomic replacement, or general
package-manager readiness claim follows from these host timestamp tests. No
release image was created.
