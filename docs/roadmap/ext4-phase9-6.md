# EXT4 Phase 9.6 — Physical acceptance preparation

2026-10-07: artifact preparation and automated readiness review **PASS**;
physical acceptance **PENDING**. The first Dell boot rejected admission before
filesystem publication; USB discovery reported zero connected root ports.
Phase 9.5 is complete. No physical USB write or power interruption has been
performed by the agent.

## Authorized first target

User selected Dell Latitude 5590 and explicitly authorized erasing this stick:
Windows disk 1, USB `Generic Flash Disk`, reported serial `C`, capacity
4,026,531,840 bytes (3.75 GiB). Recheck identity, capacity and USB bus before
flashing; the generic model/serial and disk number are insufficient alone.
See the [procedure](../plans/EXT4_PHASE9_6.md). Power cuts remain a separate
case-specific authorization.

## Dedicated artifact and boundaries

The ordinary `src/include/ext4_physical_fixture.h` defines no activation.
Only an isolated snapshot replaces it with a new PARTUUID and the authorized
capacity. Explicit `ext4_physical=start` or `ext4_physical=verify` boot tokens
select this test path. The ordinary build rejects those tokens without journal
publication. The separate QEMU fw_cfg gate remains QEMU-only.

The physical path rejects wrong identity/capacity, non-512-byte sectors,
competing fixture flags, ambiguous partitions, RO/degraded GPT and ineligible
durability before filesystem mutation. It admits the existing explicit
journal fixture after flush preflight; production journal dispatch is unchanged.
It skips the QEMU NVMe diagnostic/mutation suite entirely. Internal NVMe remains
excluded. Existing cache, barriers, taint, locks, DMA quarantine and USB policy
are unchanged.

The image is capacity-matched, with both GPT copies at the real stick boundary:
64 MiB ESP; 32 MiB bounded 4 KiB journal fixture at LBA 133120; remaining space
unallocated. Limine BIOS installation reduces the GPT entry-table count; tools
must use the actual header count and array location, not assume 128 entries.
The original pending journal/orphan seed is retained for real-device recovery.

Candidate: `physical-workspace-7hnhj2c4/physical-artifact/fortress-ext4-9.6-dell5590.img`
under `.codex-remote-attachments/ext4-phase9`.

- Data PARTUUID: `a5ba97dd-f368-41d8-ad0a-1601e6dac8d0`.
- Image SHA-256: `98c45a8ef28e9708d12775dad8abb9464aee4e52d6e791c2446d6166b2ca0279`.
- Kernel SHA-256: `ce1c72defcf0fd8ea9b356e6666e9930cc91290a72524282164540ca4b458b48`.
- Original seed SHA-256: `0c472af8174c732555bfcec81f2415b8f1f5e3d97a8739027318afbd7b4536d6`.

Each boot verifies the bounded namespace/truncate/pin/reuse fixture and actual
AP append when multiple CPUs are present. A framebuffer banner after quiet
boot is cleared reports START/VERIFY PASS, AP coverage and durability. The
user can inspect this without a serial cable. This is a test image, not a
production rollout.

## Preparation verification

Final candidate readiness passed 22/22 accepted cases: six BIOS/UEFI raw-USB
boot cycles and sixteen admission controls. Native BIOS/UEFI raw-USB boots
exercise three cycles each; separate ISO-booted controls check wrong target,
RO, capacity, duplicate target, degraded GPT, absent opt-in and ordinary build
activation rejection. Controls observe both USB write callbacks with hardware
breakpoints and hash the filesystem before/after.

Supplementary ineligible-durability controls explicitly inject READ_ONLY into
the discovered BOT policy through the debugger. These are policy-adapter
tests, not actual USB cache-probe failures. Physical classification still
requires observations on the selected stick.

Retained attempts:

- `physical-validation-iq34uve5`: initial candidate BIOS persistence and three
  admission controls passed; malformed-GPT duplicate control could not boot
  through the modified data disk's Limine loader. It is a failed harness
  attempt, not filesystem acceptance.
- `physical-validation-b01n3fy9`: final candidate main run supplied six successful
  raw-USB boots and the accepted wrong-target/RO/capacity controls. It stopped
  at the UEFI degraded-GPT control because kernel discovery saw consistent GPT
  and reached a write callback instead of rejection. The attempt remains
  retained; this is not accepted degraded-GPT evidence. UEFI may repair GPT
  before kernel discovery, so the repeated control explicitly corrupts the
  backup after firmware and before kernel USB probing.
  Negative controls now boot a separate matching ISO so malformed data GPT
  cannot prevent kernel startup.
- `physical-validation-7wijg0c9`: BIOS/UEFI duplicate GUID controls PASS with
  CRC-correct tables using the actual post-Limine entry count/locations.
  Earlier duplicate mutations assumed 128 entries and therefore also damaged
  GPT CRCs; those earlier results do not prove the unique-match check alone.
- `physical-validation-gep3byhh`: BIOS/UEFI debugger-injected ineligible policy
  controls PASS, zero write callback hits and unchanged filesystem images.
- `physical-validation-t9hxxrx5`: BIOS/UEFI degraded-GPT controls PASS when the
  corrupt backup is applied after firmware, before kernel USB probing.
- `physical-validation-n6j6s8l1`: BIOS/UEFI absent-opt-in controls PASS.
- `physical-validation-y1t_6h_l`: BIOS/UEFI ordinary-build activation rejection PASS.

`readiness-review.json` in the artifact directory verifies exact unique accepted
coverage, final bytes/AP records, immutable rejections, source hashes, permitted
storage argv and the untouched original image SHA-256. Supplementary controls
replace earlier flawed duplicate/degraded controls; rejected attempts are not
silently counted as passes. The initial read-only fsck of the deliberately
pending seed exits 4; its raw findings are retained. Journal-only recovery on
a separate Linux copy followed by `e2fsck -fn` passes. The original seed/image
is unchanged, so physical startup still exercises journal/orphan recovery.

The candidate artifact and matching build-source snapshot are retained in
`verification-93_4f_uo`. Image decompression SHA-256 agrees with the original;
`phase9-6-readiness-review.json` is retained alongside the source review.
Original campaign directories, including failed attempts, remain present.

Focused ordinary-build regressions passed: three USB journal persistence boots
(`guest-journal-usb-7rrgx144`) and one native ordered-write failure/recovery case
(`guest-usb-fault-uexmpu_2`). An earlier ordinary-snapshot regression invocation
lacked the USB fixture cmdline and was stopped; it is not a test pass. The
correct USB-configured ordinary snapshot is `guest-workspace-nwfglcnq`.

The ordinary-build control uses `guest-workspace-d8udka3d`; final physical
build is `physical-workspace-7hnhj2c4`. Matching source/binary hashes and exact
commands remain in their workspace/campaign manifests. The initial physical
build without the visible banner is `physical-workspace-i09gm3ds`.

## First operator step

### First Dell observation — 2026-10-07

The user reported flashing the candidate and booting the Dell Latitude 5590.
The photographed diagnostic is `[EXT4 PHYSICAL] REJECT
target/geometry/exclusivity; no filesystem...`; `/mnt` was not published.
The subsequent USB log photograph shows controller `0000:00:14.0`, Intel
`8086:9D2F`, successful controller reset and No-Op completion, followed by
`total=0x12 usb2=0xC usb3=0x6 connected=0x0` and
`Root port scan complete (no devices attached)`.

Local original photographs are retained under the chat attachment directory:
`01a1065a-e692-77b2-8e2b-1c5d80714dea/ad595846-0de0-4f1c-b796-ffa0e297d136/1-image-1791380541283.jpg`
and `01a1065a-e692-77b2-8e2b-1c5d80714dea/e5093a9a-c9e1-427a-bbef-2fa453c2d372/1-image-1791380678233.jpg`
under `.codex-remote-attachments`.

This attempt supplies no journal recovery or physical persistence acceptance.
The discovery log explains the missing USB block target; it does not identify
why the controller reported no connection. Source review finds that root-port
discovery samples connection status once per port and skips disconnected ports
before its power/reset sequence. This is a possible diagnostic lead, not a
confirmed cause or justification to change the driver. The next operator check
is a clean power-off, another built-in USB port, and a fresh START boot with the
stick attached before startup. No admission guard has been relaxed.

The user subsequently confirmed that the previously working image also fails
on this stick, while the usual test stick works. Mint photographs identify
`058f:6387 Alcor Micro Corp. Flash Drive`, direct USB 2.0 root-port attachment
at 480 Mbps. The [USB startup follow-up](usb-alcor-startup.md) tracks the bounded
attachment-wait correction and replacement candidate separately. The original
candidate remains retained; no physical journal pass is inferred from Mint.

1. Reconfirm the selected USB with `Get-Disk`, including `IsBoot`/`IsSystem`.
2. Flash only the reviewed candidate in raw/DD mode to that authorized stick.
3. On the Dell 5590, use F12 to boot it in UEFI mode and choose
   **EXT4 9.6 DISPOSABLE TEST - START** for the first boot.
4. Photograph the result banner and report any rejection/failure before doing
   downloads or further tests. Run `sync`, then `poweroff` for a clean stop.
5. On later boots choose **VERIFY**, not START. The menu currently defaults
   to START after ten seconds, so select VERIFY before that countdown expires.

Subsequent byte/hash, mutation/reboot and unmounted Mint audits follow the
reviewed procedure and actual first-boot observations. Clean persistence,
seeded physical recovery and deliberate interruptions are distinct gates;
none is marked passed before physical evidence exists.

## Disk-space retention cleanup — 2026-10-07

At the user's request, completed physical test image copies were compressed,
decompressed and checked against original sizes/SHA-256 before removing raw
copies. `physical-image-compression.json` and `physical-image-cleanup.json`
under `.codex-remote-attachments/ext4-phase9` record 72 images and 214.25 GiB
reclaimed. Reports, logs, manifests, unique failure bytes and current flash
images remain retained. Some historic review inputs now require restoration
from their recorded gzip paths before running the review again.

The user requested broader reduction of legacy EXT4/JBD2 outputs too. This
uses the same verify-before-removal rule and separate
`legacy-image-compression.json` / `legacy-image-cleanup.json` ledgers. Runtime
seed images, current flash candidate and active test workspaces are excluded.
Retain one verified compressed copy rather than indefinitely retaining both
raw and compressed versions of completed image evidence.

The user then explicitly instructed immediate deletion rather than waiting for
further archiving. Legacy compression was interrupted and 5,794 old raw images
were deleted (241.90 GiB reclaimed), followed by 511 additional completed image
copies and superseded physical candidates (35.21 GiB). The ledgers are
`expedited-image-cleanup.json` and `extra-old-image-cleanup.json`. Existing
archives, logs, reports, seeds and the latest physical candidate remain. Some
discarded raw images have no verified retained byte copy; do not claim that
every historical image can still be reconstructed. Any gzip produced by the
interrupted pass requires verification before use. Historical test results
remain results of their original runs, not fresh post-cleanup reviews.

## Old 4 GB stick diagnostic follow-up

The 2026-10-07 Dell 5590 diagnostic boot detects the Alcor 058f:6387 stick and
admits its WRITE_THROUGH profile but fails during journal replay with EIO and
no mount publication. Last BOT state identifies WRITE(10) data phase with no
matching completion reported. The photograph, diagnostic implementation,
focused verification and bounded slow-write candidate are recorded in
[the USB follow-up](usb-alcor-startup.md). A five-second per-phase write budget
is under physical evaluation; this is not yet an accepted hardware fix or a
Phase 9.6 pass. Production journaled RW remains disabled.

### First physical clean-shutdown audit — 2026-10-07

After the photographed START pass, the user confirms `sync` and `shutdown`.
The read-only Windows capture script checks USB capacity, non-system identity,
512-byte sectors, approved PARTUUID, offset and length before reading the
32 MiB data partition. Capture:
`physical-start-capture-8022c222570447a0b0b4bbe666a4b1e1/after-start.ext4`,
SHA-256 `8585a9401eed372b6a8813ee82167bbbb8cb121e69135b2dbbd4dd0c43c2540e`.

Independent `e2fsck -fn` exits 0 (20/512 files, 1082/8192 blocks).
The existing Linux audit on an owned GPT wrapper confirms clean filesystem
state, superblock/journal checksums, empty journal, zero orphan head, exact
journal-persist/journal-later/integration-dir/reuse bytes, removed namespace
entries, and 200 unique intact records in each independent/shared append file.
The initial generic QEMU audit expected `ring-later.bin`, which its QEMU shell
creates separately; that command was not run physically. The physical audit
explicitly checks its absence instead. This is a harness expectation correction,
not a filesystem failure. `audit-result.json` and Linux logs retain the PASS;
the captured source hash is unchanged and no physical writes or repairs occur.
VERIFY boot and the remaining physical gates are pending.

### Physical VERIFY success — 2026-10-07

User photograph
`58fb3305-71a3-4670-b07f-0fcbd853565b/1-image-1791394876951.jpg`
shows `EXT4 9.6 disposable journal fixture: VERIFY PASS`, namespace/truncate/
pins/reuse PASS, AP append PASS, WRITE_THROUGH durability, internal NVMe
excluded and an interactive shell. This follows reflashing the diagnostic
continuation image containing the previously audited physical START capture.
It establishes a successful physical verification of that restored state;
it must not be represented as uninterrupted persistence across those reflashes
or proof that intermittent USB enumeration failures are resolved. The next
clean-shutdown capture/audit and remaining physical gates are pending.

### Post-VERIFY physical capture audit — 2026-10-07

After the instructed clean shutdown, the user captures the same approved USB
data partition at
`physical-start-capture-277316195f8e4af085ffe39974df69fe/after-start.ext4`.
The script retains its generic START filename; this is the post-VERIFY capture.
SHA-256: `4bda1e232c3294d94aa0a4c1d6d2926e11a3c1b0a7abe0ddd4de5b969128b921`.
Linux read-only audit PASS: `e2fsck -fn` exits 0, clean state, empty journal,
superblock/journal checksums, zero orphan head, exact persisted fixture payloads,
namespace cleanup, and 200 unique intact records in each independent/shared
append file. `ring-later.bin` is explicitly absent because no corresponding
physical shell write was requested. Audit logs and `audit-result.json` reside
beside the capture. The capture hash remains unchanged; no repairs or physical
writes occurred during the audit. This validates the successful VERIFY output;
remaining physical gates and repeated enumeration stability remain open.

### Subsequent VERIFY without reflashing — 2026-10-07

Following the successful post-VERIFY capture/audit, the user boots the same
stick again without reflashing. Photograph
`d26cfd8d-a6c2-4950-a7ac-4514556d8f3a/1-image-1791395611160.jpg`
shows VERIFY PASS, the approved PARTUUID with WRITE_THROUGH admission,
boot 2 replay/bytes/namespace PASS, namespace/truncate/pins/reuse PASS and
SMP APPEND PASS. This is evidence of persistence across this clean shutdown
and subsequent boot, unlike the earlier capture-restoration transition.
Two consecutive successful VERIFY runs do not establish that the preceding
intermittent USB enumeration issue is universally resolved. Download/hash,
explicit overwrite/delete persistence and controlled-interruption gates remain.

### Physical HTTP downloads and post-shutdown audit — 2026-10-07

Windows serves deterministic 1 MiB/16 MiB fixtures on LAN TCP 8080. User reports
both wget downloads and guest SHA-256 checks PASS: 1 MiB under one second,
16 MiB approximately ten seconds. Timings are operator observations, not a
calibrated benchmark. After sync/shutdown, the read-only capture is
`physical-start-capture-7b7ea60926064cfbafcf124290a45afe/after-start.ext4`,
SHA-256 `e6ee5ecf7ed2bb2d05343f9959a9020152a5d6342913e54c3f0dcc8a3fdd25a6`.
The existing read-only Linux clean-state/journal/namespace/append audit PASS;
e2fsck exits 0. Independent debugfs dumps verify exact fixture bytes and lengths
for `/test-1M.bin` (1048576 bytes, SHA-256
`470952a05336a638e11755d028432cb890c3240d0b33668038a975e7e3b5b4ef`)
and `/test-16M.bin` (16777216 bytes, SHA-256
`71b83c780eeb1a2665d5aabf71229490ae069450ef9b88ca91d1229fe17f45c6`).
The first dump attempt used the suggested lowercase filenames; the actual guest
filenames contain uppercase M. Correcting this host expectation gives PASS,
recorded in `download-audit.json`; no physical writes or repairs occur.
Guest hash verification after the next boot, explicit overwrite/delete
persistence and controlled interruption remain pending.

The user subsequently reports both `/mnt/test-1M.bin` and
`/mnt/test-16M.bin` SHA-256 checks PASS after the requested VERIFY boot without
reflashing. This is operator-confirmed guest download persistence, supported
by the preceding independent exact-byte capture audit. Explicit overwrite/
delete persistence and controlled interruption remain pending.

### Explicit overwrite/delete shutdown audit — 2026-10-07

The user executes the requested `before` write/sync, overwrite with `after`,
create/sync/remove of `delete-test.txt`, final sync and shutdown, then supplies
capture `physical-start-capture-29ba256df4824841a839b7c0baff2fd8/after-start.ext4`.
SHA-256 `18f6ce8f52264776c410dc0615f42212b0f848671c27dabba7e4e2bbb2ad4ffc`.
Independent read-only Linux audits PASS: clean fsck/state and empty journal,
existing fixture/append bytes, both exact download payloads, exact
`/overwrite-test.txt` bytes `after\n`, and absent `/delete-test.txt`.
`overwrite-delete-audit.json`, `download-audit.json` and Linux logs retain the
results; source capture remains unchanged. Post-reboot guest overwrite/delete
confirmation and controlled interruption gates remain pending.

The user subsequently confirms PASS for the requested VERIFY boot without
reflashing: `cat /mnt/overwrite-test.txt` returns `after`, while
`cat /mnt/delete-test.txt` reports a missing file. Combined with the preceding
independent capture audit, explicit overwrite/delete clean-reboot persistence
is accepted for this tested Dell 5590/4 GB WRITE_THROUGH fixture. Seeded
recovery, namespace/append, downloads/hash persistence and explicit overwrite/
delete checks have passed. Controlled physical interruption remains pending;
earlier USB enumeration failures are retained and no general speed/stability
or power-loss guarantee is claimed. Production journaled RW remains disabled.

## Durable-commit pause test added — 2026-10-07

The isolated-build-only `FORTRESS_EXT4_COMMIT_PAUSE_TEST` introduces a one-shot,
mount-specific callback after successful JBD2 commit and before checkpoint.
It is undefined in the shared physical-fixture header. The explicitly selected
`ext4_physical=cut-commit` retains target admission, creates one empty regular
`/mnt/cut-commit.txt`, displays the durable-commit milestone and terminally halts
with filesystem exclusion retained. Failed commit cannot reach the milestone.
Conflicting START/VERIFY/CUT modes reject before filesystem writes.
No physical interruption is authorized or performed by adding this test.

Isolated strict build PASS. `python3 scripts/test_ext4_physical_commit_pause.py
/mnt/c/Sources/FortressOS/.codex-remote-attachments/ext4-phase9/physical-workspace-u01zo8r1`
passes BIOS and UEFI in `physical-commit-pause-_qfdolib` (2/2, zero errors).
Each case observes the pause, terminates disposable QEMU, proves the file is
absent from home metadata, recovers it as an empty regular file via independent
Linux journal replay/fsck on a copy, then independently boots FortressOS VERIFY
on the unrepaired pending image. Linux/byte/namespace/append/clean-state audits
PASS after Fortress recovery. This is emulation evidence, not physical power loss.
Full QEMU disk copies are discarded after recording results; pending partition
captures, Linux replay copies, logs and manifests remain.

The concrete first-case protocol and evidence boundaries are in
[the commit-cut procedure](../plans/EXT4_PHASE9_6_COMMIT_CUT.md).
Physical artifact review, operator approval for the exact interruption and
execution remain pending. Interrupted recovery and durable open-unlink are
separate later cases; production journaling remains disabled.

### Physical commit-cut artifact ready for case approval

Prepared artifact:
`physical-commit-cut-artifact-qxucf69w/fortress-9.6-commit-cut-dell5590.img`,
SHA-256 `9d66e300c8478b6521586a2eb0e9e3c40a29f5b0b40c67cafc330706cd92ce6d`.
It preserves the audited overwrite/download capture
`18f6ce8f52264776c410dc0615f42212b0f848671c27dabba7e4e2bbb2ad4ffc`.
The boot menu defaults to RECOVERY VERIFY; COMMIT PAUSE is a separate explicit
entry. The source/kernel/initramfs hashes, approved target and GPT identity are
retained in its manifest. Exact-artifact emulation in
`physical-commit-pause-vfesyecd` PASS 2/2 BIOS/UEFI with independent Linux and
Fortress recovery, and the read-only capture audit adapter smoke PASS.
Independent Windows full-image SHA agrees. Full disposable guest disk copies
are discarded; pending/recovered partition evidence and logs remain.

The proposed physical action is to select COMMIT PAUSE on Dell 5590 with the
approved 4 GB stick, wait for the exact durable-commit-before-checkpoint marker,
hold the power button until the Dell powers off, then unplug the stick to
remove possible standby USB power. Capture on Windows before any recovery boot.
`scripts/audit_ext4_commit_cut.py` preserves the source and replays only a Linux
copy, verifying the empty cut file and both downloads. Only after that audit
should the original stick boot RECOVERY VERIFY without reflashing, followed by
clean shutdown and another capture. Explicit operator approval of this exact
power-cut case is still required by the Phase 9.6 plan. No physical interruption
has been executed or claimed, and the other interruption cases remain pending.

### Interruption procedure readiness clarification

The three cases are specified in [the case procedures](../plans/EXT4_PHASE9_6_INTERRUPTION_CASES.md). Only durable commit before checkpoint is implemented and emulation-verified. Interrupted recovery and durable open-unlink still require dedicated hooks, artifacts, audit commands and exact-artifact verification. No approval request or physical execution for those cases is warranted yet. No physical power cut has occurred.


### First physical commit-cut capture — independent replay PASS

Operator supplied the exact COMMIT DURABLE / CHECKPOINT NOT STARTED photo,
then supplied the pre-recovery Windows capture after the instructed interruption.
Capture: `physical-start-capture-69ad78f0a97442a59335a24affe4f471/after-start.ext4`.
SHA-256: `6fa08ac23194ac783854a6289e20107ef753d912c2d235bf564a26265c889c46`.
The operator verified artifact SHA agrees with the prepared manifest.
Invocation: `python3 scripts/audit_ext4_commit_cut.py <capture>` under Ubuntu-24.04.
Exit 0, PASS: home metadata lacks `/cut-commit.txt`; independent Linux replay
on an owned copy recovers a regular empty file; both download lengths/hashes
remain exact. Original capture hash unchanged, no physical writes by the audit.
Logs and `cut-audit.json` are beside the capture. Physical FortressOS recovery
on the unrepaired stick remains the next gate; this is not yet a complete case.

### Commit-cut continuation and final physical audit — PASS with retry history

After the preserved pending capture, physical boot attempts failed USB EP0
configuration-header enumeration before filesystem access (photos
`ebe5f627`, `faa8248c`). A COMMIT PAUSE boot subsequently reached the filesystem
and rejected `cut fixture must be fresh` (`0ac24f62`): recovery may already have
occurred on that boot, so the later VERIFY is not claimed as the first replay.
Following a port-change instruction, photo `f0393e0d` shows VERIFY PASS,
namespace/truncate/pins/reuse PASS, AP append PASS and WRITE_THROUGH eligibility.
The actual successful port is not identified by this photo. Operator confirmed
`/mnt/cut-commit.txt` exists, length zero, then sync/shutdown and Windows capture.

Final capture: `physical-start-capture-710ea0bbcd044d3ab8cedaf7d7bfc7af/after-start.ext4`;
SHA-256 `18e98882443427a0e2b98914c032c7f868db7404666ac7ac28c1d0f0d507c5c4`.
Invocation: `python3 .codex-remote-attachments/ext4-phase9/audit-final-cut.py`
under Ubuntu-24.04. Exit 0: read-only e2fsck -fn, clean state, empty journal and
zero orphan head; metadata/journal checksum, fixture bytes/namespace, both exact
200-record append files, recovered regular empty cut file, both download hashes,
`after\n` overwrite and deleted-name absence PASS. Original capture unchanged.
`ring-later.bin` is explicitly absent in this physical fixture. Initial audit
adapter assumed that QEMU-only file existed; corrected the fixture expectation
and reran, with no filesystem repair or physical writes. Final audit JSON/logs
remain beside the capture.

The first physical durable-commit-before-checkpoint case passes for this tested
Dell/stick combination, including independent Linux replay of the original
pending capture and subsequent FortressOS recovered state. Enumeration failures
and intervening COMMIT PAUSE boot are retained limitations; this does not establish
reliable USB startup or general power-loss conformance. Interrupted recovery and
durable open-unlink cases remain unimplemented/pending.
