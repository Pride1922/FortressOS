# EXT4 USB transport performance follow-up

## Flush polling audit — after E4-A acceptance

The user reports Linux Mint `dd` with direct I/O and one final fsync writes
16 MiB in under one second, versus FortressOS's 112 seconds. This motivates
further profiling, but does not isolate flush latency: the operations differ
in batching and number of durability barriers.

Current BOT completion wait inventory:

| Completion | Path | Polling |
| --- | --- | --- |
| Configure Endpoint / recovery command TRBs | `send_command` | Shared `bot_poll_delay`: 10 us for first 2 ms, then 1 ms; nominal 500 ms budget. |
| READ/WRITE CBW, data and CSW | `submit_normal_trb` -> `wait_transfer_event` | Same helper; nominal 1000 ms budget per phase. |
| SYNCHRONIZE CACHE CBW and CSW | `xhci_scsi_sync_cache` -> `xhci_bot_transfer` -> `submit_normal_trb` | Same helper; no data phase or separate coarse completion wait. |
| REQUEST SENSE / recovery EP0 status | BOT transfer / `wait_transfer_event` | Same helper. |

`usb_block_flush` passes the controller's same `rings_io`, with `delay_us`
installed. The direct 1 ms delay in `xhci_scsi_test_unit_ready` is a bounded
command-failure retry pause, not a completion wait or normal flush phase;
it remains unchanged. Thus the proposed missing flush polling fix is already
present in the accepted image, and no production polling change is warranted
by this audit.

Added explicit SYNCHRONIZE CACHE host tests: two delayed 50 us completions
require 100 us total and zero coarse waits; 3500 us completions exercise fine
polling then coarse fallback; missing completion retains the existing nominal
1001 ms timeout and transport-failure latch. `make test-xhci-bot-host` PASS
under ASan/UBSan, including existing flush rejection/sense/retry tests.
No command sequence, timeout, barrier or durability contract changed.

Next performance work requires measured read/write/flush counts and latencies
on the actual driver path before assigning the remaining time to flushes.
The requested under-2-second / under-10-second hardware targets are not yet
established; this test-only change cannot improve the accepted binary's speed.

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

## Measured-path profiling image

The follow-up instruments each submitted BOT command with serialized raw TSC
reads, grouped as READ(10), WRITE(10), SYNCHRONIZE CACHE(10), and other SCSI
commands. Reports include command counts, requested bytes, failures, accumulated
TSC cycles and backwards-clock anomalies. A command that fails preflight or is
rejected by an offline transport is not counted as submitted. REQUEST SENSE and
Unit Attention retries are separate commands. ASSUMED_WRITE_THROUGH barriers
that submit no SCSI flush therefore do not appear as flush commands.

Counters are bounded static storage, use atomic field updates/sampling, and
introduce no allocations, waits, extra device operations or new locks. Existing
BOT serialization still applies; atomic counters do not make BOT concurrent.
Report fields are sampled individually, so exact deltas require quiescent I/O.
Raw cycles are not calibrated milliseconds and include host/controller wait,
copy and recovery costs inside the BOT operation. TSC migration/rate limitations
apply; a backwards clock increments anomalies rather than a huge duration.

Explicit SYS_SYNC appends four cumulative rows to dmesg after filesystem locks
are released, even if sync fails. There is no per-command printing and no console
output from this reporter. Counters persist since controller initialization;
subtract before/after snapshots rather than assuming the first snapshot is zero.
This is a diagnostic image, not a throughput fix or a journaled image.

For the Dell retest, use the newly built optional image (2026-10-03 22:57:31),
SHA-256 `834713f61712197d657740e3c73fc35647c75c492ae60ec4038faf8c979a5916`,
PARTUUID `D5E403C2-6E3F-48F1-AEDA-87AD6C2853C6`. It replaces the regular build
artifact; the earlier accepted physical image's identity remains historical.
Reflashing the designated disposable USB replaces its data; copy needed evidence
elsewhere before flashing. Default fortress.img remains ext2.

Run the test without concurrent disk tools and capture both snapshots externally
(photo/copy), so saving a log to USB does not contaminate the measured interval:

```text
sync
dmesg | tail -n 6
wget -q -O /mnt/profile-1m.bin http://192.168.0.153:8000/data-1m.bin
sync
dmesg | tail -n 6
```

Record elapsed wget time too. Verify the size/hash after capturing the second
snapshot. Counter differences give actual command amplification; cycle differences
show the proportion of BOT time spent reading, writing and flushing. Comparing
that total with wall time distinguishes BOT cost from time elsewhere, provided
TSC frequency is separately established. No promised hardware target follows
from synthetic polling tests.

### Barrier audit

The E4-A engine writes a durable dirty marker once, initializes zero/new images,
flushes NEW, writes and flushes ALLOC, then writes and flushes REFERENCE. Pending
frees remain held until references are durable; release then requires a final
ALLOC write/barrier. Successful writes and shutdown keep their existing durability
contract. Current implementation also deliberately initializes storage durably
before publishing allocation ownership.

A combined NEW/ALLOC barrier is an optimization candidate, not implemented here:
it must preserve initialized data and ownership before reference publication and
must audit partially persisted allocations, stale-byte exposure, checksums and
failure taint at every cut. Existing synchronous-sector failure tests alone do not
model reordered volatile writes. A separate disposable cache-loss/tear oracle and
ordering proof are required before changing this ordering. Delayed/batched journal
commits likewise require an explicit durability contract and complete Phase-8
metadata/orphan coverage. These measurements determine which work gives the most
benefit without guessing that 98% of physical time is flush latency.

Verification: strict kernel build / make image-ext4 and BOT/GPT/mount ASan/UBSan
PASS. BOT tests additionally verify actual command accounting for successful
reads/writes, flush retries, sense commands, timeouts and offline rejection.
Delivered-image BIOS/UEFI smoke verifies on-demand dmesg rows, no reporter console
output, sync followed by another mutation, clean shutdown and independent Linux
bytes/fsck PASS in both BIOS and UEFI; retained evidence
`build/ext4-usb/run-svgtfvrh`. A preliminary smoke runner attempt used an
unsupported QMP pipe keycode; the corrected test invokes dmesg directly and
the failed workspace is retained separately, not counted as a pass.

## First Dell command-profile snapshots

User supplied before/after photographs for the profiling image's 1 MiB test.
Transcribed cumulative values (all shown failures and clock anomalies are 0):

| Class | Before commands / bytes / cycles | After commands / bytes / cycles | Delta commands / bytes / cycles |
| --- | --- | --- | --- |
| READ | 28 / 47104 / 13019163 | 623 / 2008064 / 337802926 | 595 / 1960960 / 324783763 |
| WRITE | 0 / 0 / 0 | 520 / 2126848 / 1177500204 | 520 / 2126848 / 1177500204 |
| FLUSH | 3 / 0 / 563861 | 201 / 0 / 240370815 | 198 / 0 / 239806954 |
| OTHER | 4 / 72 / 1428109 | 4 / 72 / 1428109 | 0 / 0 / 0 |

Total measured BOT delta: 1742090921 raw TSC cycles. WRITE accounts for
67.59%, READ 18.64%, FLUSH 13.77%. Mean cycles/command: WRITE 2264423,
READ 545855, FLUSH 1211146. This disproves the proposed 98% flush attribution
for this interval; the first commentary reading incorrectly grouped a digit
in the flush counter and was corrected before this record.

The 198 flush commands include the selected before/after interval's file
creation and explicit sync, not only 64 body-write batches. Requested write
traffic is approximately 2.03 MiB and reads 1.87 MiB for a 1 MiB payload:
metadata traffic and command amplification matter. These counters include
failures if present; none are shown. They do not give calibrated seconds or
account for TCP, filesystem CPU work outside BOT, or scheduler delays. Elapsed
time for this particular run and an established TSC rate are still needed to
compare measured USB time against the complete download.

Eliminating all flush time would remove only 13.77% of measured BOT time in
this sample. A one-third flush reduction alone would remove about 4.59%,
assuming other costs constant. Consequently barrier merging is not supported
as the primary explanation or a promised route from 112 seconds to under 10.
Prioritize actual write/metadata amplification and end-to-end attribution;
retain current barriers while the transaction and failure-ordering work is
verified. Journal batching is still a design target, not a measured speedup.

The user confirms this profiling download took approximately **7 seconds**.
The next diagnostic image adds a boot-only raw TSC rate estimate using ten
existing bounded PIT 1 ms delays with IRQs already excluded by boot-probe
contract. Programming/polling overhead is included, so the reported rate is
explicitly approximate and is not used for timeout or scheduling decisions.
If calibration fails, the estimate is 0 and raw cycle reporting still works.
Dividing BOT cycle deltas by this estimate permits approximate end-to-end
attribution without treating a nominal CPU core frequency as the TSC rate.

Latest diagnostic image: 2026-10-03 23:10:57,
SHA-256 `618343ea446e3fba9a456f545f8d800266c3102f8044b2748bbaf48285e53f89`,
PARTUUID `4914F2C9-9647-4231-B9C5-C3B1E901BEB9`. The new
`[USB PERF] PIT tsc-hz-estimate=...` row precedes the four command rows.
Capture this row along with before/after counters. Calibration and counter
updates preserve device commands, filesystem barriers and SYNC_BACKED policy.

## Calibrated repeat and network-only control — 2026-10-04

User supplied snapshots with PIT TSC estimate 1971896600 Hz. Approximately
0.38 seconds are spent in BOT commands for this 1 MiB interval; network-only
wget piped to wc also takes approximately seven seconds. Investigate network
pacing before changing storage barriers. See [TCP work hints](net-tcp-poll-hints.md)
for exact latest rows, implementation and acceptance limits.
