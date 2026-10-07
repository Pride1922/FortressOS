# EXT4 Phase 9.5 — Journaled disposable USB verification

Automated gate PASS, 2026-10-07: 144/144 declared main cases, 10/10 separate
admission controls, relevant regressions and read-only evidence review.
Production journaled RW remains disabled. Physical acceptance is Phase 9.6.

## Implementation

An explicit `opt/fortress/ext4_journal_usb_test` QEMU fixture flag connects
the existing bounded journal mount fixture to the actual xHCI/BOT USB block
adapter. The boot path requires the QEMU xHCI PCI identity, excludes NVMe
and competing filesystem fixture flags, selects one exact PARTUUID, requires
primary-consistent GPT, checks the existing durability classification and
performs a successful flush before journal admission. Rejection cannot fall
back to the production mount path. Production USB selection and eligibility
are unchanged.

The former NVMe fixture body now accepts the selected partition explicitly;
both transports reuse its existing VFS, sync, freeze and AP integration tests.
Two USB-only negative fixture flags assert actual write/sync errors, subsequent
write rejection, failed sync and failed freeze after uncertain I/O. They do
not substitute fake block callbacks for the USB transport.

Filesystem, journal, driver, scheduler and synchronization implementations
were not changed. The accepted non-journaled E4-A path, ext2 default, metadata
cache, bounded IRQ-excluded polling, durability barriers and DMA quarantine
contracts remain in place.

Builds used owned source snapshots `guest-workspace-r15e9z7m` and
`guest-workspace-zwm23w_9`. The latter adds the negative fixture assertions.
Immutable per-campaign ISO/ELF hashes are checked against their respective
builds, rather than treating two distinct builds as one identity. Only the
isolated Limine configuration adds the fixed disposable USB PARTUUID; the
shared configuration and shared build outputs are not changed.

## Declared results

All USB matrices use BIOS/UEFI, 1/2/4 KiB filesystem blocks and native
512-byte logical sectors. Normal persistence and recovery interruption
include wrapped 2 KiB journal placement. SMP=4 AP append checks observe
actual AP debugger identity and independently verify 200 distinct records
in each final 3,200-byte file.

| Campaign | Result | Evidence directory under `ext4-phase9` |
| --- | --- | --- |
| Normal persistence, SMP=1/4, three boots each | 12/12; 36 boots | `guest-journal-usb-9lpsdcgh` |
| Durable commit and checkpoint after one home write | 24/24 | `guest-usb-crash-lqhs2g04` |
| Replay after one home write before publication | 12/12 | `guest-usb-crash-y6w7emcp` |
| Shared/independent AP append interruption | 12/12 | `guest-usb-append-crash-9s4ke5b5` |
| Durable open-unlink with live descriptors, SMP=1/4 | 12/12 | `guest-usb-orphan-crash-53aynd29` |
| Six native failure/removal profiles | 72/72 | `guest-usb-fault-2nz4jry3`, continued from `guest-usb-fault-btljklz5` |
| RO, wrong/duplicate GUID, degraded/conflicting GPT | 10/10 | `guest-usb-admission-p9kxg2p3` |
| NVMe journal integration regression, SMP=4 | 6/6; 12 boots | `guest-nvme-regression-lq3h4l6b` |

Normal boots exercise namespace changes, growth/truncation, pinned unlink,
allocation reuse, sync followed by later writes and successful shutdown freeze.
The third boot verifies persisted results, removes the two later-write files,
syncs and shuts down. Independent extracted-image audits check exact bytes,
namespace, clean superblock state, cleared recovery/orphan state and
`e2fsck -fn` after each boot.

Each interruption records DWARF-derived writer state and the actual debugger
stop. Linux journal-only recovery on a copy establishes committed bytes before
read-only fsck. Fresh FortressOS recovery is stopped before fixture rewrite,
and must agree with the independent result. Open-unlink cases additionally
compare allocation bitmaps and free block/inode counters exactly with Linux.

## Native failure mechanism and results

The six profiles are admission flush, ordered-data write, journal descriptor
write, checkpoint home write, mid-session sync flush and active-transfer
USB removal. QEMU's `file -> blkdebug -> raw -> USB BOT` graph injects backend
errors through the actual USB callbacks. Hardware breakpoints select the
operation. A host zero-write to an already-zero, unallocated GPT gap sector
arms the next configured backend event while the guest is stopped; it does
not alter filesystem bytes. The shared-write backend permission is explicit
and confined to the owned disposable image. Exact argv review rejects extra
storage, host-device backends, NVMe and snapshots.

Admission flush errors publish no mount and leave the extracted filesystem
byte-identical. Mutation failures reach native EIO/taint assertions, reject
later mutation/sync/freeze and retain dirty recovery-required state. Linux
and fresh-guest recovery agree on empty data for ordered/journal/removal
failures and committed 16 KiB data for checkpoint/sync failures. Final normal
execution passes independent clean-state, byte and fsck checks.

Removal occurs after a WRITE(10) transfer is submitted. Each case observes
terminal transport failure and unchanged nonzero bounce/ring ownership
addresses. This is a retained-state check, not a complete native PMM bitmap
audit or a physical DMA proof. The actual BOT sanitizer regression supplies
separate allocation, polling, timeout and layout coverage.

The initial full matrix passed 21 cases, then a recovery boot timed out in
the pre-filesystem SMP work-stealing startup selftest. Its serial log records
that failure before USB mount. The failed attempt and image remain retained;
it is not silently counted as a pass. A continuation validates prior binary
hashes and exact case-prefix coverage, references those 21 successful cases,
and reruns the interrupted case plus the remaining cases. The resulting
72-case ledger has no errors. Calibration also retained an earlier failure
where the host arming command lacked backend write permission; the runner now
requires successful arming before accepting any native result. The six-profile
calibration `guest-usb-fault-yfe1tcgy` passed before matrix expansion.

## Regressions and reproducibility

In the final isolated workspace:

- E4-A production USB: BIOS/UEFI × SMP=1/4, four cases and 12 boots PASS,
  including Ring 3 downloads/hashes, namespace, sync and offline audits.
  Retained output: `build/ext4-usb/run-hlqf8xgz` and `e4a-usb-regression.log`.
- Existing ext2 USB: BIOS/UEFI RO checks and three-boot persistence PASS,
  including unmounted fsck. `ext2-usb-regression.log` retains console evidence;
  the existing runner's temporary ext2 images are deleted on completion.
- Actual BOT/SCSI and USB mount-policy ASan/UBSan targets PASS:
  `bot-host.log` and `mount-host.log`.
- Fresh NVMe journal integration: six configurations and 12 boots PASS,
  including namespace/truncate/pins/reuse and actual SMP append.

Invocation patterns, run from `/mnt/c/Sources/FortressOS`:

```sh
python3 scripts/test_ext4_journal_usb.py WORKSPACE --matrix --integration
python3 scripts/test_ext4_guest_crash.py WORKSPACE --matrix --usb
python3 scripts/test_ext4_guest_crash.py WORKSPACE --matrix --usb --recovery-only
python3 scripts/test_ext4_guest_append_crash.py WORKSPACE --usb
python3 scripts/test_ext4_guest_orphan_crash.py WORKSPACE --usb
python3 scripts/test_ext4_usb_journal_admission.py WORKSPACE
python3 scripts/test_ext4_usb_journal_fault.py WORKSPACE --calibrate
python3 scripts/test_ext4_usb_journal_fault.py WORKSPACE --matrix
python3 scripts/test_ext4_usb_journal_fault.py WORKSPACE --matrix --continue-from PRIOR_RUN
python3 scripts/test_ext4_nvme_isolated.py WORKSPACE
python3 scripts/review_ext4_usb_journal.py
```

`WORKSPACE` is one of the explicit isolated snapshots above; the continuation
uses the final snapshot and its initial matrix directory. Makefile targets
require `EXT4_USB_JOURNAL_WORKSPACE` and do not rebuild shared outputs.

`phase9-5-review.json` verifies unique expected coverage, source-build binary
identity, exact permitted storage argv, AP records, independent recovered
bytes, orphan allocation identity, immutable rejections, failure assertions
and final clean-state checks. It retains hashes of review inputs, regression
logs, tool versions and `dumpe2fs` fixture provenance including filesystem/
journal UUIDs, features and geometry. Review performs no guest execution.

The main evidence archive is `verification-1p643qew`, and the E4-A USB
regression archive is `verification-9i_uoz3t`, under
`.codex-remote-attachments/ext4-phase9`. Image compression checks SHA-256
after decompression; original campaign directories remain present. Matching
build-source snapshots and the review are retained alongside the archive.

## Remaining limits and handoff

This finite campaign demonstrates the bounded supported journal profile
through emulated USB. QEMU termination retains host page cache; it does not
simulate physical loss of volatile device cache. False flush success, cache
loss, tears and reordering remain separately calibrated host evidence.
Synchronous flush observed in QEMU does not certify a physical stick.
4096-byte sector adapter evidence remains host-only in this phase.

No physical drive was prepared or written. Dell 5590/5530 acceptance, actual
stick/controller durability and any deliberate power-cut procedure remain
Phase 9.6. Reconnection/hot-plug recovery and production journal activation
are outside this gate. E4-B overall completion awaits that separate acceptance
and a separately reviewed production decision.
