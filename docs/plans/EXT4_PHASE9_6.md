# EXT4 Phase 9.6 — Evidence review and physical acceptance

Started 2026-10-07. Automated Phase 9.5 is complete; physical acceptance is
pending. This procedure preserves production journaled RW being disabled.

Dedicated first-target image readiness passed on 2026-10-07: six emulated
boot cycles, sixteen rejection controls, independent audits and focused
ordinary-build regressions. See the [artifact and handoff](../roadmap/ext4-phase9-6.md).

First target authorized by the user on 2026-10-07: Dell Latitude 5590;
Windows USB disk 1, `Generic Flash Disk`, reported serial `C`, capacity
4,026,531,840 bytes (3.75 GiB). User explicitly permits erasing this stick.
The generic identity requires rechecking capacity, USB bus and boot/system
exclusion immediately before flashing; disk number alone is insufficient.
This consent authorizes preparation/flashing for this target, not an
unspecified power-cut experiment. No physical writes have been performed.
See the [9.5 report](../roadmap/ext4-phase9-5.md) and
[Phase 9 acceptance scope](EXT4_PHASE9.md#96--evidence-review-and-physical-acceptance).

## 1. Review and prerequisites

Verify the 9.1–9.5 ledgers, independent Linux audits, retained failed attempts,
matching source/binary identities and verified image archives. Record the
bounded filesystem/journal feature profile, supported geometry, metadata/data
credits, orphan limits and unsupported operations in the acceptance report.
The pre-mount work-stealing timeout remains a separate
[SMP follow-up](../subsystems/smp.md#open-follow-up-pre-mount-work-stealing-startup-timeout).
The corrected backend arming permission problem is a harness issue; neither
attempt is evidence of a journal defect. Preserve both attempts.

Start with one Dell and one identified sacrificial USB stick. Record Dell
model, firmware mode, physical port, stick vendor/model/serial/capacity and
logical sector size. Record the operator's explicit authorization to erase
that exact stick. A USB 2.0 and USB 3.0 stick are available, but neither is
implicitly selected. Device names or disk numbers alone are not identity.

Read-only identification on Windows:

```powershell
Get-Disk | Select-Object Number,FriendlyName,SerialNumber,BusType,Size,PartitionStyle,IsBoot,IsSystem
```

Disconnect unrelated removable storage. Internal NVMe remains excluded and
unmounted. Preparing regular image files is separate from flashing physical
media. Do not provide or execute a destructive disk command until the target
identity and the exact artifact have been reviewed.

## 2. Dedicated physical test artifact

The Phase 9.5 fw_cfg fixture deliberately requires QEMU's xHCI identity;
its ISO cannot activate physical journal tests. Do not weaken that gate or
reuse the non-journaled E4-A image as journal evidence.

Prepare a separate isolated physical-test build and labelled boot image after
the target is identified. A build-time test-only gate and an explicit boot
opt-in must admit only the baked-in data PARTUUID. Keep the production dispatch
and default image unchanged. Use the existing journal fixture API with explicit
disposable/recovery/RW admission, unique USB parent/partition selection,
primary-consistent GPT and eligible durability before any recovery write.
Never select a target by device order or a general `rw` option alone.

Use a new fixture identity, not the 9.5 common UUID. Include kernel/ISO/image
SHA-256, source snapshot, firmware configuration, filesystem and journal UUIDs,
feature masks, block/sector geometry and initial Linux fsck output in the
artifact manifest. Start with the accepted bounded 4 KiB filesystem profile
and 512-byte sectors if supported by the chosen stick; reject unsupported
geometry explicitly. Keep the journal seed and independent expected bytes.

An image smaller than the physical stick can leave backup GPT at the image
boundary. Match the generated GPT to the confirmed target capacity, or use a
separately reviewed, target-specific relocation procedure. Do not silently
relax degraded-GPT policy. No filesystem conversion or resizing is needed.

Before physical use, test the dedicated build on owned QEMU USB images:
eligible target, absent/wrong/duplicate target, read-only mode, degraded GPT,
ineligible durability and test opt-in absent. Rejecting cases must perform
zero filesystem writes. Test clean persistence and seeded recovery on the
new image. Also verify the ordinary build cannot activate the physical gate.

## 3. First physical gate: clean persistence

Boot the labelled physical test entry on the identified stick. Capture the
selected PARTUUID, GPT status, filesystem/journal identity, controller/port,
USB speed, durability class and actual journaled RW admission. Any mismatch,
RO fallback, taint or transport failure stops the case and retains evidence.
Successful flush acknowledgement is an observation, not proof that a stick's
cache survives loss of power. ASSUMED_WRITE_THROUGH cannot certify power loss.

Three boots on the first device:

1. Recover the seeded journal/orphan, verify the expected result before test
   mutation, then create/write/append, truncate, rename, unlink with live
   references and reclaim/reuse through the bounded integration fixture.
   Verify AP identity and exact shared/independent append records on SMP=4.
   Run `sync`, make a later mutation, sync again and shut down cleanly.
2. Verify every persisted byte/namespace result before rewriting. Download
   the deterministic 1 MiB payload first and check its exact length/hash;
   proceed to 16 MiB if the first transfer succeeds. Record elapsed times and
   responsiveness without converting performance observations into a gate
   invented after execution. Exercise overwrite/delete, sync and clean reboot.
3. Verify hashes, overwrite/delete persistence and cleanup; power off cleanly.

After each stopped guest, retain a read-only image of the designated USB data
partition before any Linux recovery changes. On unmounted copies, record
`dumpe2fs`, `e2fsck -fn`, exact dumped bytes, inode links, orphan state, free
counts/allocation ownership and empty-journal/clean-state observations.
Keep read-only inspection distinct from a Linux recovery experiment. Never
use a repair pass as evidence that the original output was consistent.

The previous E4-A Dell acceptance remains separate. Physical E4-B results
cannot be inferred from QEMU or from non-journaled physical tests.

## 4. Recovery and interruption gate

The [three case procedures](EXT4_PHASE9_6_INTERRUPTION_CASES.md) define milestones
and expected recovery results. Only case 1 has verified instrumentation; cases
2 and 3 still need their hooks, artifacts and audits before approval.

First run deterministic seeded recovery on physical media and verify the
recovered bytes/namespace before fixture rewrite. This exercises recovery
through real USB without claiming a physical interruption.

Any deliberate power cut or active-transfer removal requires a separate
explicit authorization for the identified stick, Dell and exact case. Before
asking, prepare the concrete test build, observable milestone, expected old/new
result, capture method, restart procedure and independent audit commands.
Do not ask the operator to unplug at an unspecified time.

Begin with one durable-commit-before-checkpoint interruption, one interrupted
recovery and one durable open-unlink case only when the physical milestone
instrumentation is verified. Capture an image before the recovery boot;
Linux recovery runs on a copy, then fresh FortressOS recovery must match before
further mutation. Preserve failures and uncertain outcomes. Active removal
expects safe terminal failure/quarantine, not reconnection support.

Do not equate a clean reboot with power-loss recovery. If no reliable milestone
or eligible physical durability is available, record that gate as pending and
publish the narrower clean-persistence/seeded-recovery results.

## 5. Completion and device coverage

Record passes and limits for each actual stick/controller/firmware combination.
After the first-device vertical gate, use the second stick and/or Dell 5530
as a separately identified target; do not claim those combinations before
execution. On the 5530, record the actual controller reached by the selected
port, since controller index is not a physical-port identity.

Publish `docs/roadmap/ext4-phase9-6.md` with artifact hashes, target consent,
commands, observations, failures, audit outputs and each gate's status.
Physical acceptance remains pending until the user supplies actual evidence.
Any production rollout is a separate reviewed decision; this procedure
does not change the default ext2 image or enable production journaling.
