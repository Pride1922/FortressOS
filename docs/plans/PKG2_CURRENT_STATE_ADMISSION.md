# Immutable enrollment baseline and current endpoint admission

2026-10-24. Implements a private read-only current-state reader and native
delivery for one completed IDLE `demo` endpoint. No package mutation command,
production root publication or installation is enabled.

## Proof boundary

The existing selector authenticates a canonical **genesis bundle**. Its signed
bundle hash is not a hash of mutable current records. `pc_validate` first calls
the unchanged `ps_validate` on the separately retained immutable baseline, then
validates current records against that authenticated authority and lineage.
It does not adopt a baseline, keys, floors or trust state from the current disk.

Each current-profile fixture is a new isolated TEST enrollment whose genesis
catalog already authorizes the real demo artifact. Its selector is minted once
before current records advance. This does not rotate the previous enrollment's
catalog or retroactively authorize content under an older selector.

Two endpoints are supported by the pure reader:

- Exact canonical genesis, grant/state sequence 1, no artifact or executable,
  only when the trusted external floors permit it.
- Completed IDLE grant/state sequence 2, containing the authorized single static
  `demo` executable, with unchanged account files and closed account lineage.

The external grant/state floor pair must be exactly `(1,1)` or `(2,2)`. A floor
of 2 rejects a valid genesis rollback. These are trusted embedding inputs, not
values read from current records. There is no automatic durable floor advancement
or hardware rollback resistance in this step.

Descriptor, enrollment marker, origin and trust bytes must equal the authenticated
baseline. Only the reviewed sequence/hash bindings in grant, empty ownership table
and account record may advance. Account sequence, operation generation and
credential generation remain 1; account mutation is unsupported. Current state
must link to the canonical baseline state and verify against the unchanged signed
catalog, platform/base policy, whole FPK artifact, manifest and executable hash.
Rehashing state/grant/account records cannot authorize a different executable.

`pm_validate` and the existing FPK verifier provide artifact/manifest authorization;
their logic is reused, not replaced with checksum-only acceptance. Current bundle
framing is bounded and canonical, with exactly two enrollment signatures in this
fixed fixture profile. Output is preserved on error. The verifier bit is cleared
on semantic failure; invalid/overlapping workspace arguments are rejected before
workspace writes. Workspaces belong in caller-owned BSS/heap, not kernel stacks.

## Native embedding

The optional current-profile overlay is separate from the accepted initial-IDLE
overlay. Default production parsing, dispatch and source files remain unchanged.
The three private boot modules remain bootstrap, selector and policy.

The current policy module is a private capsule: the existing 1136-byte policy
prefix, two LE64 external floors, then the immutable genesis bundle. The capsule
is separate from measured bootstrap, avoiding a hash cycle. Module bounds and
the selector's signed baseline hash are checked. This is not a frozen public ABI,
Secure Boot, a root-delegation loader or production trust provisioning.

The native reader retains a distinct current bundle read from the projected/home
filesystem. The actual EXT4 adapter reads `/pkg/artifact.fpk` and `/usr/bin/demo`,
requiring root ownership, exact 0600/0755 modes, single links, nonzero generation,
bounded size, initialized dense inline extents and actual allocation/reference
ownership validation. The signed artifact then authenticates the actual executable
bytes. Set-id bits are rejected even when file content is otherwise correct.

The current namespace permits exactly the reserved package objects plus the proof
archive, exactly `demo` in the executable directory, and empty transaction and
documentation directories. Ordinary root/account publication remains closed.
The accepted owner transport is reused; status and IDLE recover report grant
sequence 2 and credential/account sequence 1. Boot journal replay still requires
complete projected-root validation before the write gate opens.

The endpoint fixtures are constructed offline in fresh disposable regular files.
Creating their archive/executable with debugfs does not exercise an installation
transaction, CLI install command, resource reservation or live namespace update.
The archive is retained as bounded verification evidence; general persistent-proof
accounting and removal policy remain future work.

## Exact verification

```sh
wsl -d Ubuntu-24.04 -- make -f tests/pkg_grant_candidate.mk test-pkg-current
wsl -d Ubuntu-24.04 -- make -f tests/pkg_grant_candidate.mk test-pkg-current-boot
wsl -d Ubuntu-24.04 -- python3 scripts/test_pkg_boot_admission.py --smoke
wsl -d Ubuntu-24.04 -- make -f tests/pkg_grant_candidate.mk test-pkg-enroll-records
```

- Host current reader: **43/43 vectors PASS** under ASan/UBSan, including current
  acceptance at both permitted floors, genesis at floor 1, rollback refusal,
  missing/corrupt evidence, mixed records, unsupported/non-IDLE states, account
  drift, and rehashed state/grant/account forgeries. Output-preservation and alias
  refusals are checked. Actual native file readback and independent Linux
  fsck/debugfs byte comparisons pass. Campaign:
  `.codex-remote-attachments/pkg-current-20261024-142406-451465/`.
- Native current profile: **40/40 boot/runtime cases PASS** on BIOS/UEFI x SMP=1/4.
  There are 12 clean current-root boots, four authenticated committed-journal
  replays matching independent complete-disk oracles, eight subsequent zero-write
  boots, and 16 zero-write refusals. The added current-specific refusals cover
  executable/archive corruption, genesis rollback, a fully rehashed forged
  executable state, and set-id mode. All 24 accepted roots pass Linux fsck.
  Campaign: `pkg-boot-admission-20261024-142358-883866`.
- Current native smoke: six BIOS clean/replay/reboot cases PASS,
  `pkg-boot-admission-20261024-141724-849341`.
- Legacy initial-IDLE regression: six BIOS clean/replay/reboot cases PASS,
  `pkg-boot-admission-20261024-142910-018565`. The optional current profile does
  not replace the initial-profile entry point.
- Legacy enrollment/readback/publication regression: PASS,
  `pkg-enroll-records-20261024-143242-449947`. Its default prepared bundle is
  byte-identical to the previously accepted `125427-391488` bundle, SHA-256
  `d1351dfa43493d4f1b005429463d565c6b12dca9cbf99e8c788e9979c60e0278`.
- Freestanding reader frame: **464 bytes**. Supplemental current-filesystem,
  transport and boot-info frames are at most **496 bytes**. Existing new-code
  codec limits and the approved 1152-byte verifier exception still pass.

**Preservation: PASS**, separately from runtime, for all final campaigns above.
Source/guard hashes and copied compilation inputs reconcile. The current
`current-reconciliation.json` also checks that the signed selector still hashes
the immutable capsule baseline, that both external floors are 2, and records ISO
hashes. Replay changes exactly 16 sectors with two flushes in each of four cases;
all other accepted boots and all refusals perform zero scoped writes/flushes.

As in the previous native gate, recovered filesystems retain EXT4's RECOVER/dirty
flag. Read-only fsck warns that it skips recovery; structural fsck success and
whole-image replay equality do not claim a clean published installed root.

## Retained failures

- `pkg-current-20261024-141300-889391`: compile FAIL caused by nested host `main`
  macro renaming and misleading indentation. The host entry point now has an
  explicit optional name macro; default behavior remains `main`. Separate
  preservation FAIL records concurrent iwlwifi/WLAN source edits.
- `pkg-current-20261024-141429-440575`: 41-vector runtime/native/Linux PASS;
  preservation FAIL from concurrent shared image/ISO/manifest/initramfs builds.
  The original result is retained, not relabeled. The final 43-vector campaign
  supersedes it and independently reports full preservation PASS.

Evidence prefixes without full paths above are beneath
`.codex-remote-attachments/`. Concurrent Wi-Fi work was preserved and excluded
from this change. The original production integration plan remains unmodified.

## Remaining delivery work

This proves authorized content and a narrowly closed current lineage, not the
historical execution/atomicity of an install or durable mutable ownership of
arbitrary files. It supports one artifact/executable of at most 8192 bytes each,
one catalog/build and a still-valid immutable baseline/catalog. It does not
support account updates, trust rotation, multiple packages, documentation members,
arbitrary counters, remove/update, or RESERVED/ACTIVATING/RETIRING recovery.

Next extend authenticated current-state admission and recovery to the bounded
pending transaction phases, then implement the authoritative typed plan,
reservation/escrow, durable ownership and bounded install/remove paths. Root
publication, broad installed-root validation, incompatible-kernel exclusion,
protected durable floors and production key choice remain separate gates.
No new AP-placement, signal-owner, replay-write-fault or physical durability
acceptance is claimed by this campaign.

Installation, production provisioning, release-media builds, pushes, physical
writes and public package mutation verbs remain disabled.
