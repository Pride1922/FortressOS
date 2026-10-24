# Package trust floors: step-2 evidence

2026-10-24, work started at `df3ec3f`; concurrent Wi-Fi commit `db096e9` retained.
[Implementation and threat boundary](../plans/PKG2_TRUST_FLOORS.md).
Private TEST fixtures only. No installation, production provisioning, release
build, push or physical-device write was enabled or performed. Concurrent Wi-Fi
work and the original production plan were preserved.

## Accepted evidence

All commands below ran using `wsl -d Ubuntu-24.04 --` from the repository root.
Evidence paths are relative to `.codex-remote-attachments/`; full commands,
inputs, hashes and results are in their manifests.

* `python3 scripts/test_pkg_trust.py`: PASS, `pkg-trust-host-20261024-203833-626264`.
  Fifteen compile/runtime commands, actual reader ASan/UBSan, freestanding frame
  check, valid threshold signatures, monotonic counter/hash checks, expiry,
  minimum advancement, measurements/contracts, duplicate/foreign role keys,
  signed malformed fields, preserved error output, alias refusal and all 1,024
  individual byte corruptions. Largest linked trust-reader frame: 656 bytes.
* `python3 scripts/test_pkg_trust_fs.py /mnt/c/Sources/FortressOS/.codex-remote-attachments/pkg-boot-admission-20261024-202843-344232/bootstrap.tar`:
  runtime PASS, preservation PASS, `pkg-pending-20261024-203823-549391`.
  Actual EXT4/JBD2 ASan/UBSan, authenticated current-state and owner tests,
  recovery through epoch 3, repeat without new I/O, selected interrupted
  recovery and Linux `e2fsck -fn` with no errors. The detached host output still
  has the journal RECOVER lifecycle set; its read-only fsck warning is retained.
* The same command's `trust-cuts.json`: **870/870 PASS**, 290 distinct cut
  positions under each of before-write/flush, after-write/flush acknowledgement
  loss, and 256-byte torn-sector models. Positions include every event in both
  floor transactions (pre-content events `[0,138)`, completion `[1171,1248)`),
  plus every other flush and its adjacent events. All recovered under fresh
  projected authentication and repeated without new I/O; zero terminal torn
  refusals occurred in this campaign. Per-cut assertions validate the actual
  authenticated semantic records; this is not 870 separate Linux fsck runs.
* `python3 scripts/test_pkg_current.py`: **43/43 PASS**, preservation PASS,
  `pkg-current-20261024-203931-769985`. Existing default profile regression.
* `python3 scripts/test_pkg_trust_acceptance.py .codex-remote-attachments/pkg-boot-admission-20261024-202843-344232 --old-kernel .codex-remote-attachments/pkg-boot-admission-20261024-194915-442535/workspace/bin/fortress.elf`:
  **24/24 boots PASS**, preservation PASS, base evidence
  `pkg-boot-admission-20261024-202843-344232/trust-acceptance-204053`.
  Eight actual installed-login/sudo/shutdown boots: BIOS/UEFI × SMP=1/4,
  followed by fresh reboots against independently advanced minimum 3. Sixteen
  refusal boots: BIOS SMP=1 and UEFI SMP=4 for whole-disk rollback, partial
  rollback retaining the newer floor, corrupt floor signature, signed future
  contract, wrong kernel, wrong bootstrap, equal-version hash fork and the
  previously accepted older kernel. Every refusal preserved the entire disk
  byte-for-byte. Premature offline minimum publication was also refused.
* `python3 .codex-remote-attachments/pkg-boot-admission-20261024-202843-344232/test_floor_fence.py`:
  **2/2 boots PASS**, `floor-fence/manifest.json` under that base. Actual login,
  sudo and `/bin/shell -c "echo X > /pkg/trust-floor.v1"` produce permission
  denial, then clean shutdown and independent completion-floor readback on
  BIOS SMP=1 and UEFI SMP=4. This checks ordinary root mutation, not just mode
  bits or a mocked fence.
* `python3 .codex-remote-attachments/pkg-boot-admission-20261024-202843-344232/test_producer_equivalence.py`:
  PASS. Default producer parts, witness, selector, bundle, policy and root
  authority are byte-identical to `df3ec3f` for the same independent fixture.
* Python compilation and `git diff --check`: PASS.

Private measured build:

| Input | SHA-256 |
| --- | --- |
| Kernel | `bf43f2e1c1f498f1313e4cbccf25c7863b26261d8bc7906eabeb83db9b80947f` |
| Bootstrap | `f9373db40aa2038f133dfeda21d0f775b36ec4e985d8992974552a746c5044a1` |
| Selector | `ed41d09bb1396eaa918273747341422ca41f8510882fb2c5e46e55b066a39646` |
| Initial policy | `e094b8dfe4b07e4f636709041f9da1520972de5d7b65b859bfe8f70d521175cc` |

## Retained failures and preservation

No failed campaign was overwritten or relabeled PASS:

* Pure reader attempts `pkg-trust-host-20261024-202822-528301` and
  `202923-781831` failed harness compilation (include paths, then missing digest
  linkage/unused crypto sections). `203031-932155` failed a test expectation
  that mutated an already decoded view while retaining its old wire hash. These
  harness errors were fixed; the final signed-wire and transition cases pass.
* `pkg-pending-20261024-203042-174022` failed the host lock-depth assertion: the
  new harness called floor publication without holding filesystem exclusion.
  The corrected harness takes the existing lock; the kernel hook already did.
  `203418-021871` then had runtime PASS but preservation FAIL because test
  sources were edited during the run. Final `203823-549391` has both PASS.
* The initial private build `pkg-boot-admission-20261024-202843-344232` completed
  with copied inputs unchanged but failed its source-preservation guard because
  `scripts/test_pkg_trust.py` changed during the build. Its reused immutable
  kernel/bootstrap passed two initial BIOS login/reboot boots; that early
  running-root campaign retained preservation FAIL for evolving test sources.
* Its first `trust-acceptance/` campaign timed out waiting for the wrong legacy
  refusal marker. Serial output actually showed module-admission refusal with
  zero writes/flushes and `[ROOT RECOVERY] invalid boot selection`. The runner
  was corrected to check that marker. `trust-acceptance-203644` passed all 24
  boots but retained preservation FAIL for test-source edits. Final
  `trust-acceptance-204053` passed both runtime and preservation, with no changed
  copied inputs or guarded sources.

The original `docs/plans/PKG2_PRODUCTION_INTEGRATION.md` remains unchanged,
SHA-256 `f1e8a769becaac55509a82a1633fda9c6057167588becab8c9cba91521fb1017`.
Concurrent Wi-Fi edits are excluded from this commit, not discarded to obtain
a clean tree.

## Limits and remaining delivery work

These results cover fixed external TEST authorities, the bounded pre-signed
demo recovery states, and a separately retained trusted boot checkpoint. They
do not prevent a boot administrator from rolling back both root and boot policy,
establish hardware entropy/clock/antirollback properties, or qualify physical
power loss. Online root-key rotation and production checkpoint provisioning are
not claimed. Native kernel exclusion uses this profile's new explicit private
boot contract; it does not enforce Secure Boot against arbitrary replacement
kernels. Full install/remove plans, documentation payloads, generalized resource
reservations and CLI/concurrent-writer acceptance remain steps 3 and 4.
