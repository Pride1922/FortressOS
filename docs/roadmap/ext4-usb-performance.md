# EXT4 USB transport performance follow-up

2026-10-03. User reported roughly 50 seconds to download 1 MiB to EXT4 on
the Dell 5590, versus about three seconds when streaming to stdout and fast
ext2 writes. Physical EXT4 performance acceptance remains open.

The supplied read-only bottleneck review correctly identified repeated
single-sector BOT commands, coarse event polling, and reading newly allocated
storage before zeroing it. Its allocation/flush timing breakdown is inferred
from code, not a measured device profile. The implementation does not treat
those estimates as hardware evidence. No driver edits from the other agent
were present when work resumed.

## Changes

- BOT command/transfer waits poll every requested 10 microseconds for their
  first two milliseconds, then use the existing one-millisecond fallback.
  PIT channel 2 remains bounded, restores gate/speaker control, and requires
  no interrupt or scheduler progress. Matching event pointers/slot/endpoint,
  event consumption, timeout failure and DMA quarantine remain intact.
- Optional synchronous block-run callbacks transfer at most 4096 bytes.
  Drivers without them retain the single-sector path. The entire range is
  validated before dispatch. A failed run is never retried through the
  sector fallback: a prefix may already have reached the device.
- GPT translates and bounds the complete run, preserves parent bounds and
  disables run writes wherever existing GPT policy disables sector writes.
- USB READ(10)/WRITE(10) uses one command for up to eight 512-byte sectors,
  or one 4096-byte sector, within the existing single bounce page. No new DMA
  allocations or asynchronous commands are introduced.
- EXT4 aligned reads/writes use the run path. Partial sectors retain the
  existing read/modify/write preservation. JBD2 aligned block I/O also uses
  runs so later journal integration will not reintroduce per-sector commands.
- New file blocks, unwritten conversions, fresh extent nodes and directory
  blocks stage zeroed images without first reading old contents. Existing
  initialized blocks still read and preserve untouched bytes. Allocation
  ownership proof and stale-byte tests remain in place.

The successful-write durability contract, NEW/ALLOC/REFERENCE barriers,
delayed frees, dirty marker, sync/freeze behavior, syscall limits, PARTUUID
selection and USB durability classification are unchanged. This is not deferred
writeback or a lock redesign. Filesystem/BOT I/O still runs under the existing
IRQ-excluded serialization. Shorter I/O should reduce those windows, but shell
latency and download speed require a physical retest. The remaining 192 flushes
per MiB may still be expensive on this particular USB device.

## Verification

Host tests use actual BOT/GPT/EXT4 implementations with mocks and disposable
regular images. BOT tests prove one command transfers a complete 4 KiB image,
check 512/4096 geometry and capacity/command-length rejection before submission,
verify 50-microsecond delayed completion uses no coarse waits, exercise slow
fallback and retain the 1001-millisecond nominal timeout boundary. Existing
stall recovery, short/residual/phase failures, no OUT replay and bounce guards
remain covered. GPT tests check translation, overflow, full-range rejection,
RO exclusion and failure without fallback/replay.

EXT4 runs the existing VFS failure matrix with both sector-only and run
callbacks. The run mock can accept a prefix and fail; the filesystem must
propagate failure and taint. Prefilled free/unwritten storage, partial writes,
gaps, fragmented extents, namespace operations, 1/16 MiB bytes and Linux fsck
still check data preservation and stale-byte exclusion.

Measured logical counts for the host 1 MiB / 16 KiB-batch case at filesystem
block size 4096 and sector size 512: **4096 sector writes become 512 write
runs**, each one production BOT command, with **447 full-sector read runs**.
Partial-sector reads are additional commands. Flushes remain **192**. This
proves command-count reduction, not physical throughput or flush latency.

Commands run successfully on 2026-10-03:

| Command | Result / retained evidence |
| --- | --- |
| `make image-ext4` | Strict kernel build and separate image generation PASS. |
| `make test-xhci-bot-host test-usb-mount-host` | ASan/UBSan BOT polling/run and GPT/mount tests PASS. |
| `make test-ext4-alloc-host` | Six geometries and allocation/failure/Linux audits PASS; `build/ext4-phase3/run-q0nhsr0l`. |
| `make test-ext4-write-host test-ext4-read-host` | Six geometries each PASS; `build/ext4-phase4/host-bizz6mwc`, `build/ext4-phase0/fixtures-qjzcoaup`. |
| `make test-jbd2-replay-host test-jbd2-write-host` | Reader 12 cases/48 rejects and writer 12 cases/3568 cuts PASS; `build/jbd2-replay/run-8m878ajk`, `build/jbd2-write/run-jvafdmns`. |
| `make test-ext4-usb` | BIOS/UEFI, SMP=1/4: 4/4 persistence cases, 12 boots plus four RO/degraded cases PASS; `build/ext4-usb/run-wy3vscbw`. |
| `python3 scripts/test_ext4_usb.py --smoke-only` | Delivered image BIOS/UEFI USB smoke, sync/shutdown and Linux audits PASS; `build/ext4-usb/run-9r1zk074`. |
| `make test-usb-persistence` | ext2 USB BIOS/UEFI RO checks and three-boot persistence/fsck regression PASS. |

Delivered `bin/fortress-ext4-test.img`: 136314880 bytes, local timestamp
2026-10-03 21:31:46, data PARTUUID `34a6c80f-242a-4cc9-a7e1-1a4fd6b7c006`.
SHA-256: `ad92f5219b686daa53e06ac34f9a50fb18edfa40db9cf69cbfe99b2a33a20068`.
The default `bin/fortress.img` remains ext2. Journal support remains a host
workbench and is not enabled in this production test image.

## Dell physical performance acceptance — 2026-10-03

User-reported manual tests on the Dell Latitude 5590 using the refreshed
non-journaled EXT4 USB image:

| Check | Observed result |
| --- | --- |
| 1 MiB LAN wget to USB | 6–7 seconds, matching hash PASS (previously approximately 50 seconds). |
| Clean reboot persistence | User-confirmed PASS. |
| 16 MiB LAN wget to USB | 1 minute 52 seconds (7 seconds/MiB), matching hash PASS. |
| Console during background download | Quiet `wget -q ... &` allows responsive commands; `top` shows wget running. User-confirmed PASS. |
| Independent Linux audit | Linux Mint: `/dev/sda2` unmounted before `sudo e2fsck -fn /dev/sda2`; e2fsck 1.47.0 completed all five passes without errors; `echo $?` returned 0. Screenshot evidence supplied by the user. |
| Independent Linux hashes | User confirms the downloaded file hashes also match under Linux Mint: PASS. |

The screenshot identifies the USB as `/dev/sda`, approximately 115.1 GiB,
with a 64 MiB data partition labelled `FORTRESS_E4_TEST`; fsck reports
19/512 files, 0.0% non-contiguous, 4420/16384 blocks. The internal NVMe is
shown separately. Device paths describe this Mint session only.

The performance, reported download hashes, clean reboot persistence and
console responsiveness gates PASS. This is manual physical evidence, not an
automated test result or a power-loss/journaling guarantee. The user subsequently supplied the boot admission record below, including
the complete matching PARTUUID, USB model, durability tier and GPT/mount policy.
The subsequent consolidated user report confirms physical post-sync mutation
and namespace overwrite/delete persistence PASS across all three boots; see
[complete acceptance](ext4-phase5-acceptance.md).

Non-quiet background output can overwrite the visible shell prompt, which
reappears after typing. Record this as a separate prompt-redraw issue, not a
failed quiet-mode responsiveness test. Windows Linux Reader extraction remains
unresolved; the clean Mint audit does not establish that application's support.

### Boot admission record supplied by the user

The user supplied the following extraction from the boot log saved on the USB.
This records the Phase-5 checklist item 3 admission fields; the original full
log was not re-read by the agent in this documentation update.

| Field | Boot-log value |
| --- | --- |
| USB device | SanDisk 3.2 Gen 1 |
| Durability | `SYNC_BACKED`; WCE=1, RCD=0, SYNCHRONIZE CACHE OK |
| RW eligibility | `RW eligible (every flush must execute)` |
| GPT policy | `Primary and Backup valid and consistent. Using Primary.` |
| Partitions | `sdap1` EFI and `sdap2` data, 131072 sectors each |
| Data PARTUUID | `34A6C80F-242A-4CC9-A7E1-1A4FD6B7C006` |
| Selected filesystem | `ext4` |
| Mount mode/result | `read-write`; `PASS: Mounted sdap2 read-write at /mnt` |
| NIC | I219-LM `8086:15D7`, MAC `C8:F7:50:0E:35:80`, STATUS `0x00080083` (link up) |
| TCP quiet period | Active, documented 120 seconds |

The selected PARTUUID matches the delivered image identity. GPT admission is
consistent, with no degraded RO fallback reported. This establishes the
observed final GPT state; it does not establish whether the flashing tool
relocated the backup table. SYNC_BACKED requires every requested flush to
execute; boot eligibility alone is not evidence of every later flush completing.
USB VID/PID and negotiated link speed are not specified in this extraction.


## Final acceptance status

**E4-A physically accepted — Dell 5590 — PASS (2026-10-03).** All seven
checklist items are explicitly user-confirmed. Earlier pending/failed notes
are historical checkpoints superseded by the [complete acceptance record](ext4-phase5-acceptance.md).
