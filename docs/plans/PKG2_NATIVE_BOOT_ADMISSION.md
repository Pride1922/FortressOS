# Private measured boot and projected-root admission

2026-10-24. Native acceptance for the fresh initial-IDLE TEST profile. This is
not production installed-root activation or completion of the production gates.

## Implemented boundary

`scripts/pkg_boot_admission_overlay.py` changes only a copied workspace. Default
kernel sources, boot parsing, syscall dispatch, trust provisioning and release
packaging remain unchanged. The private kernel recognizes `boot_mode=pkg-test-v1`
and retains the normal immutable-bootstrap account service. The selected data
filesystem is never published to VFS or the account service.

The boot-info overlay snapshots exactly three bounded optical modules with
distinct names and roles: `bootstrap.tar`, `selector.bin`, and `policy.bin`.
Missing, duplicate, unknown or mismatched roles refuse package admission. Module
backing uses the existing reserved kernel/module lifetime; selector and policy
bytes are subsequently copied into kernel-owned storage. The policy is the
1136-byte private x86-64 TEST transport, checked with a static size assertion,
not a frozen public boot ABI.

The actual kernel-file bytes and complete bootstrap archive are SHA-256 measured
against the external TEST policy before data-device admission. The offline
producer signs those actual hashes. Selector and policy are separate modules,
so neither participates in the bootstrap hash it authorizes. The trust boundary
starts with the trusted test loader/media: this is not Secure Boot, remote
attestation, production key provisioning or boot-administrator resistance.

A private QEMU fw_cfg opt-in is required before initializing the test NVMe
device. The runner attaches exactly one new regular-file 64 MiB disk. The kernel
requires a unique namespace, 512-byte sectors, consistent primary/backup GPT,
the exact selected partition identity/type/bounds and policy PARTUUID, and the
fixed 32 MiB filesystem geometry. No user request supplies a device or block map.

## Source-backed admission and lifetime

The private `jb_boot` path performs:

1. Module/measurement/device/GPT validation with writes disabled.
2. `e4_journal_source` and read-only `jbd2_analyze`, retaining the guard and plan.
3. `jbd2_preview_read_sector` through a read-only projected device, followed by
   actual EXT4 allocation/reference/orphan validation. Nonempty orphan state
   refuses this initial-IDLE profile.
4. Native account namespace/slot checks, protected inode/map comparisons,
   descriptor geometry/filesystem UUID checks, credential-content hashing and
   exact package-directory checks. Only the nine reserved package objects may
   exist; transaction, executable and documentation directories must be empty.
   Parent ownership/modes and dot/parent entries are checked.
5. Independent `ps_validate` authentication of the native-read bundle against
   the external policy and selector, including catalog thresholds, counters,
   hashes, credential lineage, time and initial-IDLE semantics.
6. A zero-write/zero-flush assertion, disposal of the projected engine, and a
   narrowly enabled `jbd2_replay` window. Only the block callbacks behind this
   gate can write. Replay is unlocked, as required by JBD2. The gate closes on
   success or failure; a failed/poisoned plan is not retried.
7. Reopening and revalidating the actual home filesystem before private backend
   admission. A retained engine and reusable mapping buffer live for the boot.

Projected reads run without the filesystem lock because JBD2's preview API
requires unlocked context. Runtime validation of the reopened home device uses
the existing rank-1 EXT4 exclusion; bounded heap operations use the existing
higher-rank allocator. No new lock rank is introduced. Time is rechecked on
runtime validation. No public namespace, allocation or EXEC admission is opened.

The private value transport and owner arena are reused unchanged from Gate 1:
kernel-owned request/result copies, credential rechecks, retained TCB birth
identity, exit cancellation and reaper hooks. Session state contains no user
pointers, callbacks or disk maps. Ring 3 exercises status/acquire/recover/release
twice per accepted boot. Here **recover is an IDLE validation/no-op**; filesystem
journal replay occurs during authenticated boot admission. Recovery of non-IDLE
package/account operations is not enabled by this profile.

## Exact acceptance

```sh
wsl -d Ubuntu-24.04 -- make -f tests/pkg_grant_candidate.mk test-pkg-boot-admission
```

Final campaign:
`.codex-remote-attachments/pkg-boot-admission-20261024-133520-827304/`.

**Boot/runtime: 35/35 PASS**, independently of preservation:

| Cases | Count | Observed outcome |
| --- | ---: | --- |
| Clean root, BIOS/UEFI x SMP=1/4, three boots each | 12 | IDLE sequence 1; zero scoped writes/flushes; whole disk unchanged |
| Authenticated committed journal, same four configurations | 4 | 16 sector writes, two flushes; exact independent full-disk replay oracle match |
| Two subsequent boots from each recovered disk | 8 | IDLE sequence 1; zero writes/flushes; whole disk unchanged |
| Refusals, BIOS SMP=1 | 11 | Zero writes/flushes and whole disk unchanged |

The 11 refusals cover changed kernel, changed valid bootstrap TAR, invalid
selector signature, missing selector, wrong module role, filesystem read error,
valid-checksum journal with an unauthentic projected record, foreign GPT identity,
partial enrollment, an unowned package object and a nonempty transaction
directory. Exact rejection stages are retained in `refusal-stages.json`.

The committed journal fixture is produced by Linux debugfs with CSUM_V3. Its one
known zero-UUID descriptor-slot bug is normalized and independently resealed,
as documented by the existing journal host oracle. The home grant record is
corrupted while the valid record remains in the committed log. Acceptance must
therefore authenticate the projected state before replay. The invalid variant
has valid journal checksums but an unauthentic projected grant and refuses before
any writes. The independent expected image changes only the restored home block
and the journal superblock; GPT and the unrelated sentinel partition remain exact.

All 24 accepted roots pass Linux `e2fsck -fn`. The 12 recovered-root inspections
warn that read-only checking skips journal recovery: JBD2 has emptied the log,
but this unpublished engine does not clear EXT4's RECOVER/dirty state. Structural
fsck success is not a clean mounted-root acceptance claim. Whole-image oracle
comparisons and the two zero-write reboot checks are separate evidence.

The symbol audit excludes the new verifier's signing/Argon2/entropy entry points.
Linked new-code frame checks pass: the existing approved verifier exception is
1152 bytes against 1280; other checked codec/adapter frames are at most 816
bytes. Supplemental transport, boot-info and parser frame checks pass at at most
512 bytes. These are frame measurements, not a proof of maximum aggregate stack.

## Preservation qualification and retained failures

The final campaign's **original full-tree preservation result is FAIL** and
the command exits nonzero on that guard. During the run, concurrent Wi-Fi work
changed `src/drivers/iwlwifi.c`, `src/net/wlan/wlan_mgr.c`, and five shared outputs:
`bin/fortress.elf`, `bin/fortress.img`, `bin/fortress.iso`,
`bin/fortress.manifest.json`, `bin/initramfs.tar`. Those edits/builds were not
reverted, rewritten or included in the package change.

`scoped-reconciliation.json` separately reports **PASS**: all other captured
source/guard bytes, all package/boot/filesystem inputs and all compiled copies
reconcile exactly. It records before/after hashes for every concurrent difference
and hashes of the tested ISOs. It does **not** relabel the original FAIL. Default
production code was not patched by the private overlay. The original integration
plan retains SHA-256
`f1e8a769becaac55509a82a1633fda9c6057167588becab8c9cba91521fb1017`.

Earlier campaigns remain retained:

- `130821-249774`: source snapshot FAIL before compilation, concurrent
  `src/net/wlan/wlan_mgr.c` edit; preservation FAIL.
- `131025-333744`: source snapshot FAIL before compilation, concurrent
  `tests/wifi_host.c` edit; preservation FAIL.
- `131221-010542`: compile FAIL from missing GPT header; corrected explicitly.
  Separate preservation FAIL from concurrent image/manifest builds.
- `131403-559946`: three clean BIOS boots PASS, preservation PASS; intermediate.
- `131756-972620`: six clean/replay/reboot BIOS boots PASS, preservation PASS;
  intermediate.
- `132136-238447`: 24 positive boots plus kernel-mismatch refusal passed, then
  bootstrap-mismatch harness FAIL: nonaligned TAR append prevented bootstrap
  parsing before admission. The corrected fixture appends a full zero TAR block.
  Separate preservation FAIL from a concurrent AGENTS.md documentation update.

All prefixes above are `pkg-boot-admission-20261024-`. No original failure is
deleted or relabeled.

## Remaining delivery boundary

The selector currently pins the **initial baseline bundle**. This reader must
not be used unchanged after package/account records advance: mutable current
state must be validated against authenticated baseline/transition evidence,
rather than compared blindly to the genesis bundle hash. That integration,
general non-IDLE recovery, complete resource escrow, ordinary writable-root
fences, account-service gating and installed-root publication remain open.

The exact private token is understood only by the copied parser. This does not
establish production downgrade exclusion, secure monotonic floors or resistance
to whole-media/boot-administrator rollback. No production key choice is made.
This campaign does not inject replay write failures/power cuts, certify physical
durability, or add a new AP-placement/signal-owner stress proof; SMP=4 is a tested
configuration and Gate 1's earlier lifetime evidence remains separately scoped.

Installation, package mutation verbs, production trust provisioning, release-media
builds, pushes and physical-device writes remain disabled. Only disposable
regular-file data disks and private copied acceptance ISOs were built or written.
