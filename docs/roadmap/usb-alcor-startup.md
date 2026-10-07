# Alcor USB startup detection follow-up

2026-10-07: physical fix verification pending.

The user reports that the old 4 GB stick fails with both the previously working
image and the EXT4 9.6 candidate, while the usual test stick works. Mint detects
`058f:6387 Alcor Micro Corp. Flash Drive`, directly on USB 2.0 root port 9,
using `usb-storage` at 480 Mbps. This establishes Linux detection, not FortressOS
acceptance. The supplied dmesg tail omits initial storage enumeration.

FortressOS photographs show controller reset and No-Op completion succeeding,
but the single root-port scan reports zero connections. The current scan had
no attachment settling interval after controller restart. The
[Intel xHCI specification](https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf)
describes root ports starting disconnected after reset and waiting for device
attachment signaling. The proposed correction allows a bounded 1000 ms polling
interval before scanning. A timing explanation for this particular stick is
still a hypothesis until physical retest.

The interval runs once at boot, uses the existing polling delay, aborts on
timer failure, and changes no DMA/event-ring ownership, locking, runtime
hot-plug policy or filesystem admission. It adds one second to each controller
scan. No vendor-specific exception is introduced.

Host ASan/UBSan root-port tests pass with immediate USB2/USB3 attachments,
attachment delayed by 750 ms, absent-device bounded completion and delay
failure before any reset. Existing stale test expectations (removed selection
fields and pre-SuperSpeed reset behavior) were updated to the public API and
current reset behavior. This host evidence does not prove physical recovery.

An isolated physical candidate was built and tested in
`physical-workspace-830hj9o_` under `.codex-remote-attachments/ext4-phase9`.
The earlier candidate and evidence remain retained. Physical acceptance of
EXT4 9.6 is still pending.

Replacement validation `physical-validation-r8jxjvj6` passes all 22 cases:
six BIOS/UEFI raw-USB persistence cycles and sixteen admission controls,
with zero campaign errors. USB ring and BOT/SCSI host sanitizer regressions
also pass. This is emulated regression evidence, not an Alcor hardware test.
Image SHA-256 independently rechecked on Windows:
`e4abf12a713e7a839f856565edfbcb7860b25345faaa8be0e3dba3b4738fe687`.
New data PARTUUID: `3bddef66-5ed0-4117-aea5-cf3bc34ee168`.
The replacement image is `physical-artifact/fortress-ext4-9.6-dell5590.img`
inside that workspace. Flashing it resets the disposable fixture; select START
for its first boot, VERIFY only on subsequent successful boots.

The Mint photos are copied from temporary attachments into
`.codex-remote-attachments/ext4-phase9/alcor-mint-observation` as
`mint-dmesg.png` and `mint-lsusb.png` for retention.

## Replacement including scheduler follow-up

The user requested a fresh image incorporating `91bf034a4d613c79b17971d5acad30b70b3551b9`.
`physical-workspace-x1u8ob69` includes that commit and the uncommitted bounded
USB attachment wait. Snapshot scheduler SHA-256 agrees with the manifest and
the current working source. Isolated strict build PASS.

`physical-validation-uqon329v` passes six focused BIOS/UEFI raw-USB boot cycles
with namespace/persistence, concurrent AP append, sync/shutdown and independent
Linux filesystem/byte audits; zero campaign errors. Invocation uses
`--persistence-only`. Admission controls are not rerun on this scheduler build;
the preceding candidate's 22-case campaign remains separately retained. These
results do not establish a comprehensive scheduler review or physical pass.

Latest image: `physical-workspace-x1u8ob69/physical-artifact/fortress-ext4-9.6-dell5590.img`.
Image SHA-256 independently rechecked on Windows:
`07f93adc2ceb2a29af42ec97d45e08f9e62fb10f9047825ef973fa897d71efd6`.
Kernel SHA-256: `d1ed10912b1fc9157d62dd4a93cbc0750618d5152b933ceaf74a51feac082e70`.
Data PARTUUID: `e68c8e17-8fcd-47fe-b7dd-8f070b5a81d5`.
Use START after flashing this latest image. Earlier candidates remain retained.

## Latest Dell observation

The supplied `image-1791385894784.jpg` photograph after the latest flash shows
successful BOT configuration, INQUIRY (`Generic` / `Flash Disk`), 7,864,320
512-byte sectors (3840 MiB), sector-zero verification and consistent primary
and backup GPT. Both partitions register. MODE SENSE page 0x08 succeeds with
WCE=0/RCD=0, SYNCHRONIZE CACHE is unsupported, and classification is
WRITE_THROUGH. The selected PARTUUID agrees with the latest image.

Physical admission succeeds but the fixture mount returns failure at
`ext4 journal recovery/publication` before a published filesystem is observed.
This establishes USB detection/read/admission on the latest candidate, not
journal recovery or write-persistence acceptance. The existing generic mount
failure message does not identify its failing stage; further diagnostics are
needed before choosing a filesystem or transport correction. The photo is
retained in the chat attachment tree at
`cd255ce3-b81a-4e7a-9051-d8e480669c4a/1-image-1791385894784.jpg`.

## Diagnostic candidate (2026-10-07)

The physical fixture now reports the last attempted EXT4 mount stage and
positive errno before the existing fatal diagnostic. After mount cleanup it
also prints the last BOT command, phase, completion, CSW, byte count and sense
snapshot. This reads stored state only; it performs no USB I/O and changes no
admission, transaction, taint, locking or publication behavior. Ordinary mount
callers retain the original API. A successful later command can replace the
last-command snapshot; this is not a complete transport error history.

Strict isolated kernel build and focused 4096-byte-block/512-byte-sector host
ASan/UBSan mount tests PASS. Admission, pre-publication rejection, injected
source read failure, success-stage reporting and existing lifecycle/concurrent
freeze cases passed. BIOS/UEFI negative controls in
`physical-diagnostics-y_ptezyv` both report `journal-source errno=95 published=0`
for an unsupported superblock revision, with zero USB write callback hits and
unchanged filesystem hashes. Two focused positive raw-USB boots in
`physical-validation-d8g5nbdz` PASS with AP append, namespace, shutdown and
independent Linux filesystem/byte checks. Completed raw test copies were
discarded for space; logs and manifests remain.

Diagnostic build: `physical-workspace-u01zo8r1`. It retains the approved data
PARTUUID `e68c8e17-8fcd-47fe-b7dd-8f070b5a81d5`. Windows confirms this identity
on the 4 GB USB. Its ESP has no drive letter and direct access was denied.
The user elected a fresh full flash instead of a boot-file-only update.
Physical diagnosis and Phase 9.6 acceptance remain pending the next boot.

The new capacity-matched flash image was assembled and independently checked
for primary/backup GPT CRCs, matching data PARTUUID, and exact ELF/initramfs
hashes extracted from its ESP. Image SHA-256:
`809c98eeb412d077a58801100ce24ebe8aeb6808996a62fa8db1e28bdb151677`.
Path: `physical-workspace-u01zo8r1/physical-artifact/fortress-ext4-9.6-dell5590.img`.
The superseded x1 full image was discarded after this verification to conserve
space; its manifest and historical observations remain.

## Physical diagnostic result (2026-10-07)

After flashing the diagnostic candidate, the Dell 5590 photograph
`093e91d5-7f11-4aec-a901-8b0f82738115/1-image-1791390246824.jpg`
shows successful USB block registration, consistent GPT and WRITE_THROUGH
admission again. Mount fails at `journal-replay errno=5 published=0`.
The stored BOT snapshot reports `last-op=0x2A phase=2 completion=0 csw=0
bytes=0`, `command-failed=0 transport-failed=1 offline=0`, and no valid sense.
Opcode 0x2A is WRITE(10): this identifies a transport failure during replay
writes, rather than an unsupported filesystem profile. Completion zero and
absent sense do not establish a device-reported SCSI rejection or its precise
cause. The failure does not prove that no earlier replay writes reached the
stick. Physical journal recovery and Phase 9.6 acceptance remain pending.

## Bounded slow-write candidate

The existing BOT implementation used a one-second transfer-event budget for
every phase, including WRITE(10) payload and status. The candidate allows five
seconds for each WRITE(10) payload/status phase, retaining the one-second CBW,
read, flush and EP0 budgets and the existing polling/IRQ discipline. It does
not retry the write, reset an uncertain transfer, or relax DMA quarantine.
Two status attempts remain possible only after the existing matching STALL
completion and successful endpoint recovery. These are per-phase requested
delay budgets, not one absolute five-second command deadline.

Slow flash programming is a hypothesis, not a proven physical root cause.
The photo's completion zero can also arise from a missing/mismatched event
or failed delay primitive. Physical acceptance is required; this candidate
must not be reported as an established Alcor fix before another Dell boot.

`wsl -d Ubuntu-24.04 -- python3 scripts/test_xhci_bot_host.py` PASS under
ASan/UBSan: injected two-second data completion, two-second status completion,
exact write/readback, six-second data completion rejected at the five-second
budget with one outstanding submission and no subsequent retry; existing
stall, sense, flush, bounded run and DMA-layout regressions PASS.
`wsl -d Ubuntu-24.04 -- python3 scripts/test_usb_mount_host.py` PASS, including
durability admission, sync/freeze and failed-write non-replay contracts.
Isolated `make -C physical-workspace-u01zo8r1 bin/fortress.elf` PASS.
The updated flash image SHA-256 is
`e254d9088a85d348d9fa0cf4c38eb23d04efdaeca8cbb71745d5c161b7b83f48`;
the image path is unchanged. The prior debug-only image hash above describes
the previous build, not this candidate.

Candidate BIOS/UEFI persistence smoke both PASS in
`physical-validation-e4h5o540` (two boots, AP append, namespace, clean shutdown
and independent Linux audits), invoked with
`python3 scripts/test_ext4_physical_image.py
/mnt/c/Sources/FortressOS/.codex-remote-attachments/ext4-phase9/physical-workspace-u01zo8r1
/mnt/c/Sources/FortressOS/.codex-remote-attachments/ext4-phase9/guest-workspace-d8udka3d
--persistence-only --cycles 1`. GPT identity and extracted ESP ELF/initramfs
hashes independently verified. Completed campaign disk copies discarded after
recording these results; logs and manifest retained. No physical write or
acceptance claimed by this automated run.

## SanDisk comparison boot

The user also booted the fixture image on the larger SanDisk 0781:5588.
Photographs `35fcd9df-3084-427e-8295-015f430f6556/1-image-1791390827280.jpg`
and `e6dbdd07-3e71-4a94-ab0b-867ae19fd0a8/1-image-1791390924296.jpg`
show successful SuperSpeed enumeration, block/GPT registration, caching page
WCE=1 and successful SYNCHRONIZE CACHE (SYNC_BACKED). The EXT4 message is
`REJECT target/geometry/exclusivity; no filesystem writes`. This is expected
fixture admission rejection on a different target, not journal replay failure
or validation of the slow-write candidate. The 4 GB Alcor reflash is in progress
according to the user; its next physical boot result remains pending.

## Physical START pass — 2026-10-07

Photograph `33df5ff9-950d-47e1-88dc-ab27e47bc43a/1-image-1791391612450.jpg`
confirms the approved PARTUUID, WRITE_THROUGH admission and an accessible
`/mnt` shell. The banner reports `EXT4 9.6 disposable journal fixture: START
PASS`; dmesg reports namespace/truncate/pins/reuse PASS, boot 1 recovered
orphans/sync/later-write PASS and SMP APPEND PASS. Internal NVMe remains
excluded. This is the first observed physical START success after the bounded
slow-write candidate. It supports that candidate on this run, without proving
the exact timing of the previous failed transfer or general Alcor behavior.
Subsequent reboot persistence, independent Linux audit and remaining Phase 9.6
gates are still pending; production journaled RW remains disabled.

## First VERIFY attempt: descriptor failure

After clean shutdown and the passing offline START audit, the user's VERIFY
boot is rejected before filesystem access. Photographs
`d139edd9-48c0-45d0-a3a5-d66776cca4d1/1-image-1791392077445.jpg`,
`fdc11e38-d0b0-4e3f-a88c-752978df79f5/1-image-1791392121858.jpg`, and
`851690a2-4779-4309-b47c-a02c0baa94ea/1-image-1791392151193.jpg`
show the correct VERIFY command line, attached High-Speed USB2 device on
port 0x9, then `Read Configuration Descriptor (255-byte fallback) failed`.
No supported BOT device registers; the driver reports DMA quarantined and
the physical fixture rejects target/geometry/exclusivity with no filesystem
writes. This is a USB enumeration failure, not a persisted-data or journal
verification failure. The captured clean START filesystem remains the last
independently audited state. VERIFY persistence acceptance remains pending.

The subsequent retry after the requested shutdown/unplug/reconnect procedure
fails identically. Photograph
`82ee8417-3353-4674-85ad-c2a5342ac851/1-image-1791392355999.jpg`
again shows port 0x9 High-Speed attachment, configuration descriptor fallback
failure, DMA quarantine and zero-filesystem-write admission rejection.
Its diagnostic configuration header is `09 02 B8 00 02 01 00 E0 32`
(advertised total length 184 bytes); the recorded diagnostic step is 7 and
completion is 1. The aggregate read failure therefore must be traced beyond
the printed completion code; do not label this second failure a write timeout.
Repeated power-cycle advice alone has not resolved enumeration. No VERIFY
acceptance is claimed, and the audited START capture remains preserved.

## EP0 completion ownership correction

Code inspection found that enumeration consumed/acknowledged event slots via
ERDP and then read their fields again, even though the controller could reuse
those slots. Both command and control-transfer paths now snapshot the event
before releasing it. Control completion must match the slot and EP0, must not
be an Event Data event, and must point to the submitted status TRB. Control
event draining is bounded and failed polling delays abort. A failed
configuration-header transfer no longer triggers another descriptor transfer
on DMA whose completion is uncertain; the existing caller quarantine remains.
This is a reproduced software defect, not yet proof of the physical root cause.
No reset/speed forcing or USB vendor quirk was introduced. Full-Speed and
High-Speed remain the reported controller states, not inferred capabilities.

`python3 scripts/test_xhci_dev_host.py` ASan/UBSan PASS includes simulated
controller reuse of the released event slot, configuration timeout with no
retry, wrong-endpoint completion rejection and Event Data rejection. Existing
descriptor/class/error cases PASS. BOT and port host regressions PASS. The
isolated strict kernel build PASS. Latest nano source was rebuilt and its host
suite PASS; diskbench host tests 11/11 PASS. Both binaries are verified inside
the rebuilt initramfs. The user clarified that the intended `disk` tool is
`/bin/diskbench`, introduced by commit `0a2d7e5`.

The image is now a VERIFY continuation candidate: its data partition contains
the unchanged, independently audited physical START capture SHA-256
`8585a9401eed372b6a8813ee82167bbbb8cb121e69135b2dbbd4dd0c43c2540e`.
Flashing this candidate restores that captured state; boot VERIFY, not START.
Image SHA-256:
`7634d4244518223a7b5c0597dad81b004a6ea38a6fc47660d80aa59b709398ba`.
Physical VERIFY and speed stability remain pending.

BIOS/UEFI VERIFY continuation emulation both PASS in
`physical-validation-2h9flmug`, using the audited physical START data in owned
USB image copies. Invocation: `python3 scripts/test_ext4_physical_image.py
<physical-workspace-u01zo8r1> <guest-workspace-d8udka3d>
--persistence-only --verify-only`. Each boot checks persisted bytes, AP append,
sync and shutdown with Linux audits. Image GPT, full-image hash, captured
partition hash and boot payload hashes extracted from the ESP independently
PASS. Disposable emulation image copies were removed after recording results;
logs/manifests remain. The image is ready for physical VERIFY evaluation,
which is still required before claiming the enumeration failure is resolved.

## Physical result of EP0 correction candidate

Photograph `3ad32f0f-43fd-4268-a9a9-4f7caf9b24b2/1-image-1791393881868.jpg`
shows the updated `Read Configuration Descriptor header transfer failed`
message for port 0x9, followed by quarantine and zero-filesystem-write fixture
rejection. Port 0x9 is reported High-Speed (480 Mbps) in this photograph.
The EP0 correction therefore did not resolve the physical enumeration failure.
The final aggregate diagnostic follows attempts on other root ports and cannot
be assumed to describe port 0x9; its step/completion/configuration bytes must
not be used to infer that stick's failing transfer. Further per-port transfer
diagnostics are needed. The saved clean START capture remains authoritative;
physical VERIFY is still pending.

Per-port EP0 diagnostics now print the failed port/step, failure category,
elapsed polling budget, matching completion, observed TRB and expected status
TRB immediately after that port's enumeration failure. Categories are 1 timeout,
2 failed delay primitive, 3 completion error and 4 unsupported Event Data.
Each control submission clears the prior result, preventing stale completion
values from describing the next command. These diagnostics add no USB I/O or
retry and preserve quarantine. Enumeration host sanitizer regression and
isolated strict build PASS. Physical failure classification remains pending.

The per-port diagnostic continuation image SHA-256 is
`2034f00527cc00111bcdd68df5adf04196b9bdd36c49fccaf496d71f254033b7`.
It retains the audited START capture, latest nano and diskbench. This change
adds failure reporting; it does not claim another physical fix. The newly
identified `/bin/disk` implementation on `storage-observability` remains a
separate integration task: its syscall/collector changes have not been added
to this diagnostic candidate, nor has that branch's ext2 barrier optimization.

The user subsequently supplies photograph
`58fb3305-71a3-4670-b07f-0fcbd853565b/1-image-1791394876951.jpg`
confirming physical VERIFY PASS, namespace/reuse and AP append PASS on the
WRITE_THROUGH fixture. This successful run uses the restored START capture;
it does not erase the preceding enumeration failures or establish stable
enumeration across repeated boots. Post-VERIFY shutdown audit remains pending.
