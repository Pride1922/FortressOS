# Phase 9.6: durable commit before checkpoint

This is a disposable test, not production journal activation. Physical execution
is pending explicit approval of this case on Dell 5590 and the authorized
4 GB Generic Flash Disk (PARTUUID e68c8e17-8fcd-47fe-b7dd-8f070b5a81d5).
No physical power cut has been requested or performed by this preparation.

## Milestone and expected result

An isolated build defines `FORTRESS_EXT4_COMMIT_PAUSE_TEST`. Ordinary source
headers leave it undefined. `ext4_physical=cut-commit` retains the existing
target, capacity, GPT, durability and internal-NVMe exclusion checks. Following
recovery/mount, the fixture arms one callback and creates `/mnt/cut-commit.txt`.
The callback runs only after `jbd2_writer_commit` succeeds and before the first
checkpoint call. It clears its own registration and terminally halts without
releasing filesystem exclusion, enabling interrupts, scheduling or submitting
another USB operation. A failed commit cannot reach the callback.

Visible marker: `EXT4 TEST PAUSED: COMMIT DURABLE; CHECKPOINT NOT STARTED`.
The serial marker begins `[EXT4 CUT] DURABLE COMMIT BEFORE CHECKPOINT;`.
At this marker, the commit's device barrier has completed. This is a device
durability-policy observation, not a guarantee against a lying device cache.
Home metadata must not contain the new file yet. Recovery must produce one
regular empty `/cut-commit.txt` on both Linux and FortressOS.

## Automated verification

`python3 scripts/test_ext4_physical_commit_pause.py <isolated-workspace>`
uses only owned sparse raw-USB copies, BIOS/UEFI, and explicit QEMU argv.
It observes the marker, terminates QEMU, captures the pending partition,
checks the home file is absent with read-only debugfs, runs Linux replay and
fsck on a separate copy, then boots FortressOS VERIFY on the unrepaired pending
image. Both recovery paths must produce the expected empty regular file.
Existing namespace/append/persisted bytes and clean shutdown audits also run.
QEMU termination is not a physical cache-loss experiment.

## Physical procedure to approve after artifact review

1. Verify the exact image hash and target. Flash only the approved stick.
2. Select the explicitly labelled COMMIT PAUSE entry. If it rejects, fails or
   never displays the milestone, do not execute the interruption case.
3. Only at the visible milestone, execute the separately approved Dell power-off
   action. No active-transfer USB removal is part of this case.
   Proposed action for approval: hold the Dell's power button until it powers
   off, then unplug the test stick to remove USB power even if standby charging
   keeps that port powered. Do not invoke `sync` or `shutdown` at the pause.
   Do not reboot the Dell before capturing the unrecovered partition on Windows.
4. Before any recovery boot, capture the USB data partition read-only on Windows.
   Preserve this original. Linux replay and fsck operate only on a copy.
5. Boot the recovery VERIFY entry without reflashing. Confirm the expected
   empty regular file, sync/shutdown, capture and independently audit again.

Interrupted recovery and durable open-unlink are separate subsequent cases.
The COMMIT PAUSE test must not be conflated with those cases or general
power-loss acceptance. Earlier USB enumeration failures remain documented.
