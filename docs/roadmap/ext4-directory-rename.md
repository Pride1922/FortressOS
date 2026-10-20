# EH-6 directory rename and subtree relocation

2026-10-20. Extends existing no-replace SYS_RENAME and `/bin/mv` on admitted
journaled ext4. [Design](../plans/EXT4_DIRECTORY_RENAME_DESIGN.md).

## Implementation

Same-parent rename retains parent links and moved `..`. Cross-parent relocation
updates both directory entries, both parent link counts and moved `..` within one
bounded JBD2 transaction. Metadata checksums and existing commit/checkpoint/flush
barriers apply. On-disk destination ancestry rejects cycles with EINVAL; existing
targets reject EEXIST. There is no directory replacement or new ABI.

Before I/O, iterative heap-backed preflight checks cold and cached descendant
paths. After durability, cached root parent/name and loaded descendant prefixes
are published without allocation or I/O. Retained files/directories keep identity.
DAC, sticky checks, protected account roles and recovery allocation floors remain.
Uncertain directory I/O fences namespace admission until recovery; it does not
publish a partial cache rebase. Existing rename timestamp behavior is preserved;
EH-1B remains open.

## Verification

Installed acceptance: **PASS 4/4 configurations, 8/8 boots, 8/8 Linux audits**.
Every `e2fsck -fn` returned zero; debugfs verified parent ownership and exact bytes.
Invocation: `python3 scripts/test_ext4_mv.py` (the `make test-ext4-mv` runner).
Evidence: `.codex-remote-attachments/ext4-mv-20261020-222546/manifest.json`,
per-case serial/argv/Linux logs and `rename-stack.json`. The manifest records no
shared filesystem-source or existing bin-output changes during the gate. The
private user-command baseline is commit `692c62d` (later unrelated commits after
the requested `09f45c9` base). No concurrent command changes were edited.
New/changed rename frames range from 48 to **448 bytes**, under 1024.

Host final acceptance: **6/6 journaled geometries PASS, 2280 callback cuts**
(with two recovery mounts per cut), **3/3 trusted fallback geometries PASS**,
and **21 Linux audits PASS**. Invocation:
`python3 scripts/test_ext4_mv_host.py --out build/ext4-mv-acceptance`
(the same harness as `make test-ext4-mv-host`, with a separate evidence folder).
Evidence: `build/ext4-mv-acceptance/manifest.json` and per-geometry test/Linux logs.
The source-preservation assertion passed. Cut counts by block/sector geometry:

| Block / sector bytes | Callback cuts | Linux audits |
| --- | ---: | ---: |
| 1024 / 512 | 280 | 3 |
| 1024 / 4096 | 160 | 3 |
| 2048 / 512 | 520 | 3 |
| 2048 / 4096 | 160 | 3 |
| 4096 / 512 | 1000 | 3 |
| 4096 / 4096 | 160 | 3 |

Each nonjournaled fallback geometry adds one Linux fsck audit. The Ring 3 probe's
largest frame is 32 bytes; the kernel rename maximum is 448 bytes.

Existing health/account-headroom regression: `make test-ext4-health-host`,
**6/6 PASS, 42 Linux audits** in `build/ext4-health-recovery/`. This campaign
preceded the final cycle-error precedence/fallback refresh corrections; the final
EH-6 host/guest campaigns exercise those corrections. Earlier atomic-replacement
fault regression `make test-ext4-replace-fault-host` also passed.

The host probe exercises real mounted ext4/VFS/JBD2 code under ASan/UBSan with
1/2/4 KiB blocks and 512/4096-byte sectors. It checks same/cross-parent movement,
nested files, retained cached paths and identity, cold remount relocation, cycle,
existing destination, noop/trailing slash, cold path overflow, DAC/sticky/account
rejections, and allocation failures before I/O. Cross-parent write/flush callback
cuts cover before/after completion and four persistence models, each with two
recovery mounts. Every cut checks old-or-new hierarchy, parent links, `..`, exact
payload bytes and recovery admission. Independent Linux audits inspect successful
and final recovered images; Linux is not run independently at every cut.
Three additional nonjournaled trusted fallback geometries get clean Linux fsck.
These do not establish crash atomicity or actor support for nonjournaled mounts.

The guest runner builds only kernel ELF/initramfs and a release-excluded probe in
a copied workspace. It installs that probe on disposable regular-file GPT NVMe
fixtures and boots BIOS/UEFI × SMP=1/4. Each case performs syscall operations,
`/bin/mv`, clean shutdown, independent `e2fsck -fn`/`debugfs`, reboot payload
verification, shutdown and a second Linux audit. Linux checks moved directories'
`..` against `/var` inode, old-name absence and byte-exact nested payloads.
No network, physical disk, installer execution or release image is involved.
Unrelated tracked user commands are frozen at the runner's recorded HEAD only
inside the private build. Concurrent shared source/output changes are reported in
the manifest, never overwritten or attributed to this work.

## Stack gate and limitations

The default kernel build emits stack-usage records; every rename/new move helper
is asserted at most 1024 bytes. The Ring 3 probe uses a strict 1024-byte compiler
frame warning as an error. This is a scoped EH-6 gate, not a whole-ext4 G1 claim.
An initial blanket compiler gate exposed existing oversized O0 frames in
`e4_journal_backup` (1072), `e4_admit` (1168), `ext4_journal_analyze` (1440),
`e4_open_journal_files_owned` (1168), `e4_orphan_validate` (1552), and
`e4_mount_journal_admitted` (1696). Those routines are unchanged. The failed build
is retained at `.codex-remote-attachments/ext4-mv-20261020-221449/build.log`.

Preflight caps work at 4096 entries/directories and 4096 directory blocks;
excess work or descendant path overflow returns EFBIG before writes. Directory
moves therefore remain selective compatibility, not unrestricted POSIX coverage.
Process cwd is stored as a string and is not rebased; callers inside a moved tree
must chdir to its new path. This task does not redesign process/filesystem locking.
No new timestamp policy, symlink/hardlink support, directory replacement, physical
acceptance, sector-tear or arbitrary hardware power-loss claim is made. The
callback fault model covers existing block-device persistence semantics, not
arbitrary intra-sector tears. Foreground latency under maximum subtree preflight
and simultaneous rename stress need separate measurement; SMP boot coverage alone
does not establish those properties. Existing lock ranks/IRQ exclusion and
physical-write gates are unchanged.

## Retained verification failures

The first expanded host run found a fixture omission: its protected-account inode
was not registered. Registering the fixture's account-directory role corrected the
expected EROFS check. A later complete host run passed all cases but failed its
source-preservation guard after the timestamp-scope correction; the final gate
was restarted. The first installed BIOS case passed operations/reboot but the
runner reused an exclusive audit output filename; phase-specific filenames fix
that harness error. Its artifacts remain at
`.codex-remote-attachments/ext4-mv-20261020-221851/`. Subsequent passing intermediate matrices are superseded by the final
source-hashed run, which also tests cycle precedence for an existing child and
fences a post-publication refresh failure in the trusted fallback.

Acceptance created no push or release image. The implementation was committed
subsequently at the user's request.
