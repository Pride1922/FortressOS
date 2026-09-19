Status: COMPLETED 2026-09-19.
Result: see ROADMAP.md §"Phase 9G.4 — USB Writable Persistence &
Durability Classification (2026-09-19)" and AGENTS.md §2.

This file is preserved as the planning artifact for Phase 9G.4. It
describes the intended implementation and acceptance criteria as
written before the work began. It does not describe current behavior
or outstanding work. The implemented result and its verification
evidence are recorded in ROADMAP.md.


Phase 9G.4: USB durability and writable-mount completion plan
Status: proposed implementation plan, 2026-09-19. This document does not mark
9G.4 complete or authorize writes to an unidentified physical disk.

1. Establish the failure and preserve the baseline
The latest Dell photo establishes that Kingston/Phison VID:PID 13FE:4200
enumerates, reads GPT, and mounts ext2 read-only. SYNCHRONIZE CACHE(10)
returns command-failed CSW status 1; REQUEST SENSE reports key/ASC/ASCQ
0/0/0. This is inconclusive about cache behavior. It establishes neither
successful synchronization, nor write-through operation, nor a defective
device.

Existing local work includes WRITE(10), synchronization, mount opt-in, sense
retrieval, pre-write flush probing, transfer checks, quarantine on failure,
and clean-shutdown QEMU persistence tests. Missing: MODE SENSE, complete BOT
recovery, explicit durability classification, and Dell writable acceptance.

Before implementation:

Preserve the current diff and untracked runner in a reviewable checkpoint.

Keep the unpushed 9G.3 commit separate. Do not reset, discard, or silently
squash existing work.

Capture a baseline build identifier, image SHA-256, firmware, PARTUUID, USB
identity and logs.

Record the latest photo in ROADMAP.md.

If Linux is available on the Dell, optionally obtain raw INQUIRY, current
caching-mode-page, and sense responses from the deliberately identified USB
device, without mounting it RW or changing device settings. Linux's fallback
assumptions are not device data.

The commits below are logical review units. Split further if transport changes
become too large. Advance on acceptance evidence, not a promise of one commit
per day.

2. Commit 1a — Audit and validate BOT completion handling
Suggested title: test(usb): validate BOT completion and result classification

Primary files: src/drivers/xhci_bot.c/.h, src/drivers/xhci.c,
existing endpoint/control helpers, tests/xhci_bot_host.c.

No new functionality. This commit reads existing code, adds tests that
exercise every result class, and fixes bugs the tests reveal. If the audit
finds nothing wrong, the commit is tests only.

Audit completion pointer/cycle handling, ring wrap, DMA visibility, exact
CBW/CSW sizes, residue checks, 512/4096-byte buffer bounds, and bounded
event draining.

Separate result classifications: success, valid SCSI command failure, short
data, malformed CSW, phase error, timeout. Retrieve sense only when the
transport state permits another command.

Bound the whole classification path as well as each wait.

Acceptance:

Sanitizer fault tests for every result class.

Repeated ring-wrap tests.

Late and wrong completion tests.

No DMA reuse under any failure path.

Do not add stall recovery in this commit. If the audit finds a stall-handling
bug, record it and fix it in 1b.

3. Commit 1b — Implement bounded BOT stall recovery
Suggested title: fix(usb): recover bounded BOT stalls with endpoint reset

Primary files: src/drivers/xhci_bot.c/.h, src/drivers/xhci.c,
endpoint/control helpers.

Implement the BOT-required stall handling and reset sequence, coordinated
with xHCI endpoint stop / reset / dequeue ownership.

Use the actual interface and endpoint identifiers.

Bound the whole recovery sequence as well as each wait.

Do not replace recovery with a runtime host-controller reset.

If safe endpoint recovery cannot be demonstrated, latch the device offline
and retain its DMA allocations.

Never reuse uncertain DMA buffers, sleep, enable interrupts, or wait on
another thread below the ext2 IRQ-save lock.

Do not blindly replay writes after uncertain completion.

Capture bounded diagnostic records; print only after subsystem locks have
been released.

Acceptance:

Sanitizer fault tests for stall, unrecoverable stall, removal, and failed
recovery.

No DMA reuse under any path.

Device-latched-offline state observable via the diagnostic dump.

4. Commit 1c — Prove WRITE(10) at the block level
Suggested title: test(usb): verify raw block writes on disposable fixture

Primary files: scripts/test_usb_persistence.py (raw block section),
Makefile.

Add an explicitly gated QEMU raw test on an isolated disposable scratch
disk.

Write distinct full-sector patterns, read back, and compare every byte.

Cover first, last, and rejected out-of-range LBAs.

Cover independent 512-byte and 4096-byte geometries.

No filesystem involved. No raw patterns on the physical USB or GPT
filesystem image.

Acceptance:

Byte-exact readback for every written sector.

Rejected out-of-range writes return an error, not silent corruption.

Both geometries pass independently.

This commit is the block-level proof that WRITE(10) works, independent of
error handling. If it fails, the failure is either the transfer path or the
coordinate, not the stall recovery.

5. Commit 2 — Discover cache policy and qualify synchronization
Suggested title: feat(usb): discover current SCSI cache policy

Primary files: BOT/SCSI implementation and host mocks/parser tests.

Add bounded MODE SENSE(6) and MODE SENSE(10) support for the current
caching page 0x08, subpage zero. Request current values, not
changeable/default/saved values. This deliberately extends the commands
deferred by AGENTS.md. Do not add MODE SELECT or change device cache
settings.

Use a fixed documented fallback sequence between the two forms when a
command is unsupported. Recover a stalled transport before attempting the
next command. Stop on fatal transport or device errors. No open-ended
probing.

Parse actual transferred bytes and mode-data lengths independently. Validate
headers, block-descriptor lengths, page codes, SPF/subpage layout, declared
lengths, and arithmetic before reading WCE. Respect write protection.
Missing, truncated, malformed, or conflicting reports mean unknown.

Retain raw response bytes and command/status/sense details for one
diagnostic report.

A valid current page with WCE=0 is device-reported write-through. WCE=1
is write-back. A missing page is not WCE=0.

Test SYNCHRONIZE CACHE separately with IMMED=0. Keep the current bounded
Unit Attention retry; never convert failed CSW plus NO SENSE into success.

A successful empty-device probe qualifies the command path, not physical
persistence. Test writes followed by synchronization and fresh
initialization on the disposable raw fixture as a separate acceptance step.

Acceptance:

Parser matrices for both command forms, optional block descriptors, legal
short replies, WCE on/off, write protection, wrong/subpages, bad lengths,
failed/zero sense, and conflicting responses.

One Dell diagnostic report containing all results before any
device-specific compatibility decision.

6. Commit 3 — Publish an explicit durability mode through the block adapter
Suggested title: feat(usb): gate block writes on qualified durability

Primary files: xhci_bot.c/.h, xhci.c/.h, block.c/.h as necessary.

Use a driver-owned state machine, not the mere presence of function pointers:

Evidence	Result
Healthy transport and successful cache synchronization	Synchronization-backed writes eligible
Valid current cache page explicitly reports WCE=0, no write protection or other disqualifying error	Device-reported write-through writes eligible
WCE=1 and synchronization unavailable	Read-only
Unknown or conflicting cache policy and synchronization unavailable	Read-only
Transport offline, write protection, media/error state	Reject writes; reads only if independently safe
For synchronization-backed devices, every flush must execute and successfully
complete the SCSI command. For qualified write-through devices, the block
durability barrier may complete without a cache command only after all prior
synchronous writes completed successfully and no error is pending. Document
that this is a barrier backed by device-reported write-through semantics, not
a successful SYNCHRONIZE CACHE command.

Audit block, partition, and ext2 callers.

Never silently change modes after a runtime flush failure.

Latch failures, fail affected writes and barriers, and preserve ext2's
error/taint behavior.

Invalidate cached capability evidence on reset, media change, or removal.
Unsupported reconnection remains unavailable until reboot.

Do not whitelist VID:PID alone or assume removable flash has no volatile
cache.

Add a durability diagnostic dump, printed once per device behind the same
debug flag as the USB state dump:

text
[USB DURABILITY] Device: sda (Kingston USB DISK 2.0, VID=0x13FE PID=0x4200)
  INQUIRY: response=<raw bytes>, vendor="Kingston", product="USB DISK 2.0"
  MODE SENSE(6) page 0x08: response=<raw bytes>
    WCE = 0
    RCD = 0
    Page length = 20
  MODE SENSE(10) page 0x08: not attempted (MODE SENSE(6) succeeded)
  SYNCHRONIZE CACHE test: not attempted (WCE=0, sync is a no-op)
  Classification: device-reported write-through
  Mount eligibility: RW eligible
This is the artifact that lets a future reader know why the driver made the
decision it made.

Acceptance:

Test the full policy table.

Success-after-error rejection.

Ordered barriers.

Capability invalidation.

Propagation through partition adapters.

Raw block write / barrier / readback across reboot.

Production /mnt remains read-only while this layer is reviewed.

7. Commit 4 — Integrate mount authorization and a real sync operation
Suggested title: feat(storage): enforce USB durability policy at writable mount

Primary files: usb_mount.c, enumeration/selection code, ext2.c/.h,
VFS/syscall boundary if required, user/shell.c, image builder, and mount
tests.

Write the eligibility table before implementing. The mount decision is not
"RW or RO" but a mapping from condition to outcome:

Condition	RW eligible	RO eligible	Neither
GPT primary + backup consistent, ext2 clean, durability OK	✅	✅	
GPT primary valid, backup invalid, ext2 clean, durability OK	❌ (degraded GPT)	✅	
GPT both invalid	❌	❌	✅
ext2 dirty (unclean shutdown)	❌	❌	✅
ext2 tainted (write error)	❌	✅	
Durability unknown	❌	✅	
Transport offline	❌	❌	✅
Implementation:

Require explicit usb_data_mode=rw, a selected PARTUUID, validated ext2,
acceptable GPT policy, an eligible durability mode, and a healthy device
before the first filesystem write. Keep the default boot entry RO.

Fix real multi-device ambiguity: enumeration currently stops at the first
BOT device, so injected duplicate partitions do not prove clone rejection.
Inspect all candidates, or conservatively refuse RW when unique selection
cannot be established. Preserve internal NVMe exclusion.

Resolve exact-size versus larger as-flashed media explicitly. Add a
distinct, validated eligibility policy if supporting backup GPT at the image
boundary; otherwise document refusal. Validate both relevant headers and
arrays, and reject conflicting valid copies. Never repair or relocate GPT
automatically.

Log selected identity, GUID, actual mount mode, and durability mode.

Failed eligibility may fall back to RO only if the eligibility table permits
it. Never clear a dirty marker merely to make mount pass.

Add a normal sync path that preserves a mounted filesystem's dirty state
and returns barrier failures to the shell. Do not reuse
ext2_sync_all() — it marks the filesystem clean and freezes writes for
shutdown. Keep normal sync and clean shutdown as separate lifecycle
operations.

Acceptance:

No target, wrong/malformed target, RO default, valid RW, duplicate physical
candidates, degraded/conflicting GPT, failed flush, unknown cache, write
protection, dirty ext2, and removal.

Assert zero filesystem writes before failed eligibility.

Test actual ext2 alongside mount-policy mocks.

Review any intentional lifecycle or API contract changes before
implementation.

8. Commit 5 — Prove persistence and state precisely what passed
Suggested title: test(usb): verify durability paths and crash persistence

Primary files: scripts/test_usb_persistence.py, host tests, Makefile,
ROADMAP.md, AGENTS.md evidence and status.

Retain BIOS and paired-OVMF UEFI three-boot create/read/overwrite/delete
coverage and offline e2fsck -fn after every clean shutdown.

Compare exact bytes and sizes, including shorter overwrites, empty files,
and multi-block files. Substring checks are insufficient.

Add write → sync acknowledgment → abrupt QEMU termination → fresh boot or
offline readback. Killing QEMU leaves the host cache alive; combine with
backend flush tracing and injected flush failures. Add a mock volatile
cache whose unflushed data disappears on simulated power loss. Do not
claim host-power-loss proof from a process-kill test.

A normal sync leaves ext2 dirty, and the current mount rejects dirty
ext2. For crash tests, inspect an immutable image copy offline first. If
recovery is needed for guest readback, repair only a separate disposable
copy and record that intervention. Do not make automatic repair or dirty
mounting a hidden prerequisite. An expected unclean marker is distinct from
structural corruption and from a clean-shutdown e2fsck pass.

Exercise exact-size and larger media fixtures, both durability modes, and
unsupported devices.

Validate final QEMU argv: only the disposable USB data device, with
read-only firmware code and disposable vars as firmware exceptions.

Preserve logs, firmware/QEMU versions, image hash, and build identifier on
failure.

Run BOT and mount host suites and relevant ext2, storage, shell, and power
regressions. Preserve existing fixtures and label their NVMe scope.

Dell gate: on the user-selected test USB, record cache/flush evidence,
save known contents, sync, shutdown, disconnect and reconnect the
powered-off stick, boot, and compare exact contents. Repeat with a shorter
overwrite. If the stick supplies neither usable synchronization nor explicit
write-through evidence, record it as unsupported for RW. Do not force a
passing result.

Completion requires automated passes and separately recorded Dell
acceptance for an identified device, firmware, and durability mode. Sudden
physical power-loss tolerance is a separate claim requiring separate
authorized hardware testing.

References
USB BOT 1.0

Linux SCSI cache parameters

sdparm current cache-page inspection

T10 sense keys

Use applicable SCSI SPC/SBC command and page definitions during
implementation. Linux compatibility defaults and SAT-specific proposals do
not establish this USB device's actual behavior.

