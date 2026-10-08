# Case 2 — Interrupted physical recovery

Prepared 2026-10-07 for Dell 5590 / authorized disposable 4 GB Generic Flash Disk.
Physical execution and this case's power-cut approval remain pending.

Artifact: `physical-recovery-cut-artifact-c7kpwuuk/fortress-9.6-recovery-cut-dell5590.img`
under `.codex-remote-attachments/ext4-phase9`.
SHA256: `d65d962e6463086a9a374758e372f35c550e8212334ed548c0931a5a9fb3fda8`.
Same target capacity/PARTUUID as case 1. The seed is the original, unrepaired
case-1 pending capture (SHA256 `6fa08ac23194ac783854a6289e20107ef753d912c2d235bf564a26265c889c46`).
It preserves the downloaded bytes and recorded overwrite/delete state.

## Milestone

An isolated build defines `FORTRESS_EXT4_RECOVERY_PAUSE_TEST`. Ordinary builds
do not define it. Explicit `ext4_physical=cut-recovery` arms a partition-specific
one-shot callback only after physical eligibility checks. The replay engine
writes its first unrevoked image, confirms another distinct unrevoked image
remains, successfully flushes, and calls the terminal hook before continuing
replay or clearing the journal. Failed write/flush cannot reach the marker.
No mount is published or USB transfer outstanding at the pause. The added flush
is test instrumentation, not a change to production replay policy.

Exact visible marker:
`EXT4 TEST PAUSED: RECOVERY PARTIAL DURABLE; JOURNAL RETAINED`.

Emulation verifies block offset 4096 is replayed while other changed blocks
remain old, with an active journal. Linux recovery on a copy and FortressOS
recovery converge to the regular empty `/cut-commit.txt` and accepted fixture.
The temporarily partial filesystem need not pass pre-recovery fsck.

## Operator sequence after exact-case approval

1. Verify image SHA and disposable stick identity. Flash this image.
2. Boot RECOVERY PAUSE (approved interruption test only).
3. Only at the exact milestone, hold Dell power until off, then unplug the stick
   to remove standby USB power. Do not sync or shut down normally at the pause.
4. Connect to Windows and capture with `scripts/capture_ext4_physical.ps1` in
   Administrator PowerShell BEFORE any recovery boot. Send capture path/SHA.
5. Agent runs in WSL:
   `python3 scripts/audit_ext4_recovery_cut.py <capture> <artifact-manifest>`.
   Original capture stays untouched; Linux replay uses a separate copy.
6. After audit PASS, boot original stick RECOVERY VERIFY without reflashing.
   Photograph VERIFY PASS; check `ls /mnt/cut-commit.txt` and
   `wc -c /mnt/cut-commit.txt` (exists, zero bytes).
7. Sync/shutdown, capture again; agent independently audits clean filesystem,
   empty journal/orphans, expected bytes/namespace/allocation and downloads.

USB enumeration failures stop before recovery; retain logs. Do not silently
replace the stick or reflash away evidence. This image contains snapshot nano
and diskbench; `/bin/disk` from storage-observability is not integrated.

## Verification

Strict isolated build PASS. Exact-artifact BIOS/UEFI partial pause, independent
Linux restart and FortressOS restart PASS 2/2 (`physical-recovery-pause-oq9ifre8`).
Repeated recovery boots PASS 2/2 with independent fixture audits. Capture-audit
adapter smoke PASS. Actual JBD2 replay host ASan/UBSan regression PASS in isolated
workspace (`build/jbd2-replay/run-ao61_o1x`). Windows full-image SHA agrees.
No physical execution, general hardware durability or reliable USB startup claim.
