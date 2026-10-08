# Case 3 — Durable open-unlink

Disposable Dell 5590 / identified 4 GB Generic Flash Disk only. Production
journaled RW remains disabled. Physical execution requires separate approval.

## Exact fixture

Isolated gate: `FORTRESS_EXT4_OPEN_UNLINK_PAUSE_TEST`; explicit boot token:
`ext4_physical=cut-open-unlink`. Existing target capacity/PARTUUID, GPT,
durability and internal-NVMe exclusion checks precede filesystem admission.
Conflicting physical test modes reject before filesystem writes.

The kernel creates `/mnt/cut-open-unlink.bin` with 8192 deterministic bytes
`(i*17+3)&255`, closes the writer and syncs. It opens a read reference, unlinks
the pathname, syncs/checkpoints the orphan transaction and confirms the name
is absent and all bytes remain readable through the retained reference.
Only after these synchronous operations succeed does it terminally halt:

`EXT4 TEST PAUSED: OPEN UNLINK DURABLE; REFERENCE RETAINED`

No live USB transfer remains; the reference is never closed at the pause.
Pre-recovery capture must show one traditional zero-link orphan with exact
payload, precisely two still-allocated blocks and one inode relative to baseline.
There must be no other bitmap ownership changes and no directory pathname.

After restart, live references are gone. Both Linux orphan cleanup on a separate
copy and FortressOS recovery must reclaim the orphan exactly once, clear the
orphan head, restore block/inode bitmap ownership and free counts to baseline,
leave the name absent, and preserve unrelated fixture/download bytes. A second
VERIFY boot exercises allocation/reuse again and must preserve that result.

## Operator procedure after artifact review and exact-case approval

1. Verify the published SHA256 and target identity; flash the dedicated image.
2. Select OPEN UNLINK PAUSE. Send a photo of the exact marker; failures stop.
3. After approval, hold Dell power until off, then unplug the stick to remove
   standby USB power. Do not sync or shut down normally at the pause.
4. Connect to Windows; capture with Administrator PowerShell:
   `powershell -NoProfile -ExecutionPolicy Bypass -File C:\Sources\FortressOS\scripts\capture_ext4_physical.ps1`.
   Do not reflash or boot recovery before this capture. Send path/SHA.
5. Agent audits under WSL:
   `python3 scripts/audit_ext4_open_unlink.py <capture> <artifact-manifest>`.
   Read-only original inspection precedes Linux cleanup on an owned copy.
6. After audit PASS, boot original stick RECOVERY VERIFY without reflashing.
   Verify PASS banner and absent `/mnt/cut-open-unlink.bin`; sync/shutdown.
7. Capture again. Independently audit clean state, empty journal/orphans,
   bitmap/free-count baseline and existing fixture/download bytes. Retain all
   rejection attempts; intermittent USB enumeration is a separate limitation.

Artifact identities and actual verification results are recorded in the
[9.6 evidence report](../roadmap/ext4-phase9-6.md). No physical pass follows
from QEMU or successful flush acknowledgement alone.
