# EH-2 atomic replacement — 2026-10-19

Runtime and additive syscall implemented. Acceptance details below are limited
to disposable host/QEMU fixtures. No physical-device or sector-tear claim.
Implementation and automated acceptance checkpoint. TLS/HTTPS work remains separate.

## Contract

[`SYS_RENAME_REPLACE=84`](../../src/include/syscall_abi.h) replaces an existing
regular file with another closed, singly linked regular file on the same healthy
journaled ext4 mount. Both names must exist. Existing SYS_RENAME remains
no-replace on ext4; unsupported backends have no unlink/rename fallback.

The credential-aware backend revalidates both names, directories, file types,
protected account inodes/names and DAC/sticky rules under the existing rank-1
filesystem lock. Metadata/journal credits and the fresh destination cache node
are reserved before I/O. Source durability precedes one metadata activation
transaction containing both name edits, victim zero-link/orphan intent and all
checksums. Source owner/mode, content mtime and creation time survive; source and
victim ctime and parent ctime/mtime advance explicitly.

Open victim descriptors retain their original object, bytes and independent or
dup-shared offsets. Last close/sync/freeze reclaims the orphan in bounded steps.
Cleanup failure cannot retroactively turn successful replacement into failure.
Cached source entries are retired to close the lookup-before-open writer race.
An open using a retired destination entry returns EAGAIN for a fresh pathname
attempt; successful readers see one complete version, never mixed payloads.

Uncertain activation I/O permanently taints the mount and fences new namespace,
open, readdir and EXEC admission until recovery. Validated retained regular-file
reads remain possible when the backing device can read. The victim uses its
pre-activation inode/generation snapshot because home metadata may be partially
checkpointed. Ordinary tainted-read behavior is unchanged for other operations.
No operation claims rollback after uncertain I/O.

Full [design and reviewed admission policy](../plans/EXT4_ATOMIC_REPLACE_DESIGN.md).

## Automated evidence

- `make test-ext4-replace-host`: **12/12 PASS; 12,784 modeled interruption cuts**.
  Actual mounted VFS/EXT4/JBD2 under ASan/UBSan,
  1/2/4 KiB blocks × 512/4096-byte sectors × normal/forced wrapped activation.
  Wrap cases also cross inode-table block boundaries and wrap the sequence
  number during replacement. Every activation/cleanup write/flush event is cut
  before/after under four persistence modes; recovery is repeated twice, with
  full ownership/reference admission each time. Final copies receive Linux
  fsck, namespace and exact payload audits (36 images). The cut workload replaces
  a 1031-byte victim with a 2051-byte source; large-victim reclamation is covered
  separately without cuts. Includes independent/dup victim
  handles, old/new executable snapshots, source-open rejection, delayed opens,
  same-directory edits, empty files, multi-step large-victim cleanup, no-op,
  absent/type/backend/trailing-slash/DAC/sticky/account/frozen/orphan-capacity
  denials and unchanged failure disk buffers.
- `make test-ext4-replace-fault-host`: **PASS**, ASan/UBSan; all four measured allocation
  failures and thirteen early read failures preserve disk/state without I/O;
  exhausted journal credits return ENOSPC unchanged; 32 pthread races between
  two replacers and a reader; 1024 retained legacy cache entries reject unchanged
  then retry successfully after reboot; zero free data blocks still permit
  allocation-free replacement and cleanup. Non-root source UID/GID/0600 survive
  activation in a writable directory. The 2038 ctime extension is correct while
  mtime/crtime survive, and short-extra rejection precedes I/O. Six resulting
  filesystems and two old/new activation witnesses receive independent Linux
  fsck/metadata/payload audits (eight filesystems total).
- `make test-ext4-replace`: real Ring 3 syscall through isolated BIOS/UEFI
  SMP=1/4 boots on disposable NVMe; **4/4 PASS**. NULL/kernel/wrapped/unmapped
  pointers, invalid flags, both overlength inputs, a string crossing into an
  unmapped page, a valid page-crossing string, read-only strings, missing/type/
  open-source/no-op controls, unchanged old rename, dup offsets, retained fstat,
  old/new payloads, sync/shutdown and offline Linux fsck/bytes. Probe stack frame
  maximum is 144 bytes. Probe is packaged only with REPLACE_TEST_PROBE=1.
- Regressions: `make test-fstat-host` and `make test-ext4-time-host` PASS;
  freestanding filesystem/VFS/syscall objects compile with strict warnings.

Local logs and source/binary/hash manifests: `build/ext4-replace/`,
`build/ext4-replace-fault/`.
Final Ring 3 evidence:
`.codex-remote-attachments/ext4-replace-20261019-191725/manifest.json`.
Unrelated wget/TLS/build rules were frozen at committed checkpoint `52bda4f`
inside the isolated workspace, avoiding the concurrent HTTPS edits. Replacement
sources were the current working-tree sources. Release outputs remain unchanged
during that gate. There is no normal release image build or retained secret disk.

The first isolated build failed against unfinished HTTPS include rules; the
successful gate uses the explicitly recorded unrelated checkpoint above. Early
host campaigns were superseded when publication was strengthened against delayed
opens; only the final full campaign counts toward acceptance. Harness diagnostics
also corrected a pre-mount-versus-post-mount snapshot comparison and bounded read
faults to measured read calls, rather than treating an untriggered fault as a
filesystem failure.

## Remaining scope

2026-10-20 follow-up: EH-3 durability, health reporting and account allocation
headroom implementation/verification is tracked in
[EH-3 evidence](ext4-durability-health.md). The EH-2 results below remain their
original bounded scope; EH-1B stays separately open.

This is one-file activation, not multi-file package atomicity. Package recovery,
locking, signatures, reservations, active services and base-system updates remain
their planned phases. EH-1B broader timestamp policy, EH-3 durability/health APIs
and recovery headroom, EH-4 scale, EH-5 latency and EH-6 features remain open.
No new host-key rotation, account replacement bypass, raw-write admission,
production boot ordering or physical durability guarantee was added.
Guest admission uses the journaled `/mnt` NVMe fixture; installation/update of
packages on an installed root remains a package-manager acceptance campaign.
