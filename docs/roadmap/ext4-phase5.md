# EXT4 Phase 5 - production USB integration

Integration implemented and automated gates verified, 2026-10-03. Dell performance/download/reboot/fsck checks now have manual PASS evidence below; remaining checklist items must not be inferred from QEMU results.

The selected USB PARTUUID policy now probes the superblock prefix and dispatches extent-format filesystems to EXT4, otherwise to ext2. Probe failure, invalid magic or unsupported legacy feature masks leaves /mnt unmounted. Extent-free journalled ext3/extended profiles cannot enter the legacy ext2 RW API. Full geometry, feature, checksums, clean-state and root admission remain in each filesystem. An EXT4 mount rejection never falls through to ext2. USB provenance (sda), unique selection, GPT consistency, durability classification, callback capability and preflight barrier are unchanged; physical internal NVMe remains excluded. Degraded or ineligible requested RW retains only the existing RO fallback, subject to filesystem admission.

SYS_SYNC uses ext4_sync for the selected writable EXT4 volume; successful sync neither freezes writes nor marks the volume clean. Shutdown/reboot calls EXT4 freeze/clean close and existing ext2_sync_all independently, without nested filesystem locks. Failure returns EIO before the platform action. Existing ext2 APIs and normal ext2 mid-session barrier behavior are unchanged. RO EXT4 shutdown issues no filesystem write.

The existing ext2 header says dirty RW mounts reject, but the current implementation warns and allows them. This phase does not change that API behavior. The legacy RW API also lacks a compat-mask check; production dispatch now prechecks the same legacy masks as ext2 RO admission, rejecting journalled/unsupported formats before any flush or filesystem call. EXT4 continues to reject unclean/recovery-needed volumes without repair or writes, including attempted RO fallback under its restricted admission profile.

`make image-ext4` builds `bin/fortress-ext4-test.img`, separately labelled EXT4 E4-A TEST - NO JOURNAL in the boot menu. Default `make` still builds the ext2 `bin/fortress.img`; its current menu and cmdline format are preserved. The optional image uses 4 KiB blocks, 256-byte inodes, extents/filetype/sparse_super/large_file/metadata_csum only, eager inode initialization, no journal, 64 MiB data partition and existing GPT/ESP layout. It seeds the same README/notes. A raw image copied to a larger USB may require an explicit host-side backup-GPT relocation on the designated test device to meet the unchanged strict GPT policy; otherwise RO fallback is expected. The kernel never repairs it. The image builder refuses EXT4 output over an existing path, the default image, or a device path. Generic generation requires a fresh output name. The make target stages a fresh image, verifies it and atomically refreshes only the dedicated regular build artifact; no in-place conversion/migration or hardware flash is performed.

Phase-4 limits remain: 8 GiB logical files, 32 KiB write callbacks, 64 block-image credits, bounded maps/trees/nodes, regular rename without replacement, no directory rename or active-target deletion, truncate-to-zero, no wall-clock timestamps and no journal/crash-safety guarantee. Large initialized writes can hold the filesystem IRQ-save lock while synchronous USB I/O completes; raw TSC timing is recorded by the fixture, not calibrated physical latency. Physical responsiveness is an acceptance criterion, not a claim here.

The production USB runner creates its own unique fresh boot image from the current kernel and records versions, source hash, every boot argv, UART/stderr, image hashes, clean-state and independent Linux fsck/dumps. BIOS/UEFI at SMP=1/4 each use three boots. It saves/reopens 16 MiB, downloads 1/16 MiB through Ring 3 wget, hashes all three, exercises namespace operations and deletion across boots, uses real SYS_SYNC followed by another mutation and real poweroff freeze. SMP=4 pins independent/shared append workers to separate APs; the existing validator checks all records and ordering. Exact argv preflight rejects additional drive/blockdev/USB injection. No NVMe data disk is attached to this USB test. Previous NVMe evidence remains separately labelled Phase 4.

Host policy tests cover actual USB selection code with mocked filesystem admission/durability, both sector sizes, selection exclusions, eligibility/fallback, probe failures, no EXT4-to-ext2 fallback and sync/freeze error propagation. These mocks do not establish physical I/O. The actual filesystem's zero-write admission and taint guarantees are covered by Phase-4 sanitizer tests.

Automated results:

| Command | Result / evidence |
| --- | --- |
| `make` / `make image-ext4` | Strict build and Linux image integrity PASS. Repeated opt-in generation atomically refreshes its regular build artifact; default data format verified ext2. |
| `python3 scripts/test_usb_mount_host.py` | ASan/UBSan PASS: 512/4096 geometry, USB/PARTUUID exclusions, eligibility, format rejection including extent-free journals, no ext2 fallback and sync/freeze failures. |
| `make test-ext2` | Eight host geometries PASS. |
| `make test-storage` | BIOS/UEFI storage audits PASS. |
| `make test-power test-smp-append` | Shutdown/reboot PASS; BIOS/UEFI ext2 SMP=4 independent/shared append and offline fsck PASS. Power runner corrected for current cwd prompt. |
| `make test-pipe-host` | Actual pipe/syscall validation, rollback and SIGPIPE sanitizer regressions PASS. |
| `make test-ext2-write` | BIOS/UEFI three-boot persistence, namespace and Linux fsck PASS. |
| `make test-usb-mount test-usb-persistence` | Latest selector build: host BOT/durability/mount PASS; BIOS/UEFI controller present/absent mounts and ext2 RO/three-boot RW persistence with offline fsck PASS. |
| Focused `test_ext4_write.case(...,4096,...)` | BIOS/UEFI NVMe, two configurations / six boots PASS; `build/ext4-phase5-nvme/run-cv5mys9x/manifest.json`. |
| `python3 scripts/test_ext4_usb.py --smoke-only` | Delivered latest-kernel image, BIOS/UEFI: actual small writes, SYS_SYNC followed by mutation, clean shutdown, Linux exact bytes/fsck PASS; `build/ext4-usb/run-zj3aduds/smoke-manifest.json`. |
| `python3 scripts/test_ext4_usb.py` | PASS: four RW configurations / twelve boots plus four immutable RO/degraded-GPT cases. Downloads, hashes, true AP append, SYS_SYNC/post-sync mutation, clean shutdown and Linux bytes/fsck verified; `build/ext4-usb/run-ysfd2h4i/manifest.json`. CPU-count assertions checked in all retained SMP=4 boot logs. |

The full USB fixture run began before the final legacy-feature-mask guard was added. EXT4's extent-dispatch path is unchanged by that guard; the final delivered kernel's EXT4 path is additionally verified by the smoke test, and its legacy rejection behavior by the latest host/ext2 USB regressions. All physical observations remain pending.

RO/degraded tests require partition hashes unchanged. Failed preliminary RO runs expected an unsupported QMP keycode / overly specific tool diagnostic; final assertions exercise mkdir denial, namespace absence and complete immutability. Superseded workspaces are retained. The earlier Windows-mounted full gate passed both first boots before interruption for native fixture storage; it is not full-gate evidence. A cached-sector-read host microbenchmark measured 20000 reads at 2.605 s through DrvFS and 0.007 s on native WSL storage. This explains moving the workbench; it is not a physical USB throughput claim. Native fixtures keep real barriers, exact argv and independent audits; complete evidence is copied back after the run.

## Dell acceptance handoff

### Physical performance follow-up, 2026-10-03

The user confirmed EXT4 read-write admission, consistent GPT, SYNC_BACKED
durability and a successful preflight barrier. A 1 MiB wget file download took
more than three minutes; the same HTTP body streamed to wc in three seconds,
and ext2 writes on the USB were reported fast. Physical performance acceptance
therefore failed; extraction with Windows Linux Reader also remains unresolved.

The follow-up removes empty-role flushes, writes the durable dirty marker only
on the first mutation, and skips the second allocation write/barrier when no
blocks or inode are released. Zero-initialization still has a barrier before
allocation/reference publication, frees remain delayed until references are
durable, and every I/O/barrier failure still taints. There is no delayed cache
or USB durability-policy change. Wget batches file output into 16 KiB writes (the existing syscall limit),
including body bytes already received with headers and the final EOF tail;
stdout continues to emit each receive. SYS_SYNC and clean shutdown are unchanged.

The actual VFS host test measures 192 barriers for a contiguous 1 MiB transfer
in 16 KiB writes in all six block/sector geometries (4 KiB/512: 4096 sector
writes). Previously ordinary wget receives of at most 4 KiB each required five
barriers: roughly 1280 per MiB with full chunks, often more with short receives. These counts exclude file creation/close and are logical I/O
counts, not a promise of physical throughput. The refreshed test image requires
reflashing; its newly generated PARTUUID is recorded in build/ext4-dell/image.json.
Dell timing and Linux Reader extraction need retesting after clean shutdown.

Latest Dell retest reported by the user: approximately **50 seconds for 1 MiB**.
This improves on the earlier multi-minute result but remains unacceptable;
physical performance acceptance stays failed/open. The user explicitly chose
to continue EXT4 journaling phases and revisit storage-path profiling before
final acceptance. Journaling does not constitute a performance fix.

Follow-up verification: strict `make image-ext4` PASS;
`make test-ext4-alloc-host` PASS including every commit/finish I/O/barrier cut
and independent Linux audits; `make test-ext4-write-host` six geometries PASS
with 192-barrier assertions and exact bytes/fsck
(`build/ext4-phase4/host-qqb4bj2l`); `make test-wget` host codec and
BIOS/UEFI CLI/file/stdout/error cases PASS. Delivered-image smoke tests
passed both firmware modes (`build/ext4-usb/run-c98fhine`).
The full repeated USB gate PASS: four configurations / twelve RW boots plus
four immutable RO/degraded cases, including true AP append, exact hashes,
Linux bytes/fsck, SYS_SYNC/post-sync writes and clean shutdown
(`build/ext4-usb/run-_evzhj24/manifest.json`).
An initial 32 KiB wget buffer was rejected by the existing 16 KiB socket ABI;
that failed test and superseded USB workspace are not acceptance evidence.
The final buffer follows MAX_SYSCALL_WRITE_LEN; the ABI was not changed.

Use only a designated disposable test USB. Preserve any existing data on another device before selecting the new image; this phase provides no migration utility. No flashing command is executed by the agent. Record Dell model, USB VID/PID, speed/topology, selected PARTUUID, GPT policy, actual mount mode and durability class from the boot log. Select the separately labelled test entry without verbose. Confirm Selected filesystem: ext4 and the intended unique USB target, not an internal disk.

Serve deterministic 1 MiB and 16 MiB files on the LAN and retain the server hashes. Configure networking with ifup. Download with wget -O /mnt/download-1m.bin and wget -O /mnt/download-16m.bin, check wc -c and sha256sum, sync, then verify another write still succeeds. Reopen/overwrite/append/truncate and create/move/delete regular files; use supported namespace operations only. Clean poweroff, reboot and recheck all hashes and deletions. Check console responsiveness during downloads and note errors. A power loss is not an accepted crash-consistency experiment for E4-A.

After clean shutdown, audit the test USB's data partition read-only on Linux using e2fsck -fn and independently compare file bytes/hashes. Do not run repair as proof of correctness. Record the full results before marking E4-A physically accepted. ASSUMED_WRITE_THROUGH is a disclosed device policy, not proof of power-loss durability. Journaling begins at Phase 6 and remains separate.

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
