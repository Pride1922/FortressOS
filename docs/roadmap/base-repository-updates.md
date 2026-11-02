# Repository base updates — implementation checkpoint

2026-11-02. The user approved `docs/plans/BASE_REPOSITORY_UPDATES.md` before
implementation. Runtime code, producer/publisher support, CLI integration and a
complete installer are implemented locally. Firmware and physical acceptance are
pending. No production repository publication, commit or push was performed.

## Delivered behavior

- `pkg upgrade` / `pkg upgrade -y` discover `fortress-base` independently of the
  application inventory, preview it with app upgrades, prepare it last and report
  reboot required. Source downloads precede writer acquisition. The dedicated
  base syscall operations accept retained source descriptors, never devices/LBAs.
- The privileged path verifies the signed catalog and base object, platform,
  predecessor and configuration ABI, payload hashes, required base executables
  and immutable namespaces. It rejects installed-command conflicts and checks
  catalog/root floors across the existing application inventory.
- Two complete kernel/bootstrap/readiness/release sets use fixed allocations in
  the existing ESP. Two separate alternating floor records retain the highest
  accepted catalog/release across inactive-slot overwrites and explicit recovery.
- Publication flushes and reads back inactive payloads and records before menu
  selection. Uncertain writes fence this boot. Previous installed/recovery menu
  entries remain; automatic failed-boot fallback is not claimed.
- The selected bootstrap supplies immutable base paths; ext4 retains accounts,
  settings, documents and application inventory/files. The legacy installed-root
  behavior remains outside the explicit complete-base layout.
- Boot confirmation is available through `pkg confirm-base` and the baseline's
  Order-99 supervisor service. `pkg recover-base` repairs selectors/incomplete
  inactive metadata from a verified running set without lowering rollback floors.

## Verification actually run

`python3 scripts/test_base_update.py` under the existing Ubuntu development
container passed. Evidence: `build/base-update-host-r6q5zwyb/manifest.json`.
The actual C format/catalog/boot writer runs under ASan/UBSan. It checks wrong
signatures/hashes, malformed headers, catalog-floor rejection, predecessor
zero-write refusal, a write-range oracle, 24 representative partial-prefix/flush
cuts over a 4114-event activation, complete old/new payload/record combinations,
torn confirmation, mixed-selector and incomplete-record recovery, retained floors,
and two consecutive upgrades. These are representative cuts, not exhaustive
power-loss coverage or a physical atomic-sector guarantee.

The same suite ran the actual installer deployment/provisioning engine on a
regular-file target, followed by independent `fsck.fat` and `e2fsck` checks.
Evidence: `build/root-phase4/runtime-6qsbgvf_/manifest.json`.
Disposable disk images were removed after evidence collection.

`python3 scripts/test_pkg_system_host.py` passed under ASan/UBSan, including
base-operation authorization, retained descriptor snapshots, source/output bounds,
allocation cleanup and unchanged descriptor offsets. Evidence:
`.codex-remote-attachments/pkg-system-host-20261102-064143-811453/manifest.json`.
Its storage/base-operation callbacks are mocked; they do not establish boot writes.

`python3 scripts/test_pkg_batch_cli.py` passed under ASan/UBSan, including base
preview/cancellation, app-before-base sequencing, reboot-required output, fenced
state refusal, partial-failure reporting and legacy unsupported-base behavior.
Evidence: `.codex-remote-attachments/pkg-batch-blvdwghc/`.
Catalog, network, inventory and syscall adapters are mocked in this CLI suite.

The actual VFS namespace host test passed under ASan/UBSan:
`tests/base_namespace_host.c`, evidence `build/base-namespace-host.log`.
It checks selected executables/assets, hiding removed base files, preserved
settings/apps, parent traversal, listing new route names, mutation refusal and
namespace sealing. It does not emulate firmware or block drivers.

Normal kernel/package/bootstrap builds passed. New base kernel units enforce
`-Wframe-larger-than=1024`; the largest new writer frame is 992 bytes, with the
base preflight at 896 bytes. No new QEMU campaign or physical-device writes were
performed, in accordance with the user's retained validation scope.

## Image

`bin/fortress-base-installer.img`, 553648128 bytes (528 MiB).
SHA-256: `68e591626926a60acfd5b5147f787816341dccd87691d60045ca6e54da421c54`.
Build receipt: `bin/fortress-base-installer.manifest.json`.
It records the exact kernel/bootstrap hashes and the uncommitted source state.
The media producer completed GPT/FAT/payload/hash checks. This is a built candidate,
not a firmware/physical boot PASS.

The superseded `fortress-ready-2026-11-02-installer.img` created by this chat was
removed after replacement. Concurrent unrelated desktop edits/build artifacts
were preserved. Cleanup receipt: `build/base-update-image-cleanup.json`.

[Installation/update and publisher commands](../base-repository-updates.md).
