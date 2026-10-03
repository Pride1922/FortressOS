# E4-A physical acceptance — Dell Latitude 5590

**PASS — 2026-10-03.** The user explicitly confirms completion of every
Phase-5 checklist item. This is manual hardware acceptance, separate from
host/QEMU tests. The agent received the user's consolidated report, boot-log
extraction and Linux audit screenshots; the original USB evidence files remain
on the user's test device.

## Configuration

- Dell Latitude 5590; I219-LM `8086:15D7`, MAC `C8:F7:50:0E:35:80`.
- SanDisk 3.2 Gen 1, `SYNC_BACKED`: WCE=1, RCD=0, SYNCHRONIZE CACHE OK;
  RW eligible with every requested flush required to execute.
- `sdap2`, PARTUUID `34A6C80F-242A-4CC9-A7E1-1A4FD6B7C006`;
  EFI/data partitions each 131072 sectors.
- Primary and backup GPT valid and consistent; primary used. User reports
  no relocation needed.
- EXT4 selected, read-write mode, successful mount at `/mnt`.
- `EXT4 E4-A TEST - NO JOURNAL` from `bin/fortress-ext4-test.img`, non-verbose;
  network `net=192.168.0.168/24,192.168.0.1`.
- Full `boot.log` saved on the EXT4 USB partition.

## Completed checklist

| Item | Manual result |
| --- | --- |
| 1 — Flash/boot | PASS: separate optional non-journaled image. |
| 2 — GPT policy | PASS: consistent primary/backup, RW admission. |
| 3 — Identity | PASS: full boot admission record retained in boot.log. |
| 4 — HTTP payloads | PASS: Windows Python HTTP server serving build/ext4-dell. |
| 5 — Writes/namespace/sync | PASS: 1/16 MiB downloads, exact sizes/hashes, sync followed by `echo after-sync`, mkdir, create, append, rename, readback, clean poweroff. |
| 6 — Three-boot persistence | PASS: Boot 2 retains hashes, after-sync marker and first/second content; overwrite leaves replacement only, file/directory deletion and clean shutdown; Boot 3 verifies deletions and both hashes. |
| 7 — Independent Linux audit | PASS: unmounted `/dev/sda2`, e2fsck -fn 1.47.0 completes all five passes, exit 0; independent Mint hashes match both files. |

| Payload | Bytes | Elapsed | SHA-256 (FortressOS and Mint match) |
| --- | --- | --- | --- |
| data-1m.bin | 1048576 | 6–7 s | `470952a05336a638e11755d028432cb890c3240d0b33668038a975e7e3b5b4ef` |
| data-16m.bin | 16777216 | 112 s | `71b83c780eeb1a2665d5aabf71229490ae069450ef9b88ca91d1229fe17f45c6` |

Linux fsck summary: `FORTRESS_E4_TEST: 19/512 files (0.0% non-contiguous),
4420/16384 blocks`. The user reports namespace/three-boot screen captures,
FortressOS/Mint hashes, offline fsck output and boot.log as retained evidence.
Quiet background wget permits responsive commands and concurrent top.
Non-quiet background output can overwrite the prompt; prompt redraw remains
an independent shell issue.

## Performance and limits

The transport changes improve the reported 1 MiB timing from approximately
50 seconds to 6–7 seconds (roughly 7–8x); 16 MiB sustains 7 seconds/MiB.
The host 16 KiB-batch workbench counts 192 flushes/MiB. Approximately 36 ms
per flush and 98% of elapsed time are inferred attribution, not measured
physical flush timings. Merge-to-128 and journal-to-about-4 proposals are
unverified optimization targets: transaction batching, barriers, checkpoints
and metadata coverage must be designed and measured before claiming those
counts or speeds. Phase 8 remains the next integration unit; no barrier has
been removed on the basis of these estimates.

A later Mint handoff exposed `not clean` superblock state even after an
unmounted read-only fsck returned 0. FortressOS rejected both RW and RO mounts
under its existing clean-state admission rule. An unmounted interactive
`e2fsck -f` completed without displayed structural errors and changed the
state to clean; a direct reboot then mounted RW and sync worked. This records
filesystem admission, not a confirmed controller fault. The earlier offline
read-only integrity audit remains separate from that later state update.

E4-A is physically accepted for this bounded non-journaled profile on this
machine/device. It does not establish power-loss safety, journal production
support, other USB/device acceptance or persistent installation readiness.
