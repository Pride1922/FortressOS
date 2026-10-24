# Running enrolled root and live account-service handoff

2026-10-24. Completes the remaining item-1 handoff for the accepted private
Gate-1 fixture profile. This is actual running-root publication and the actual
global account service, not the earlier detached readiness preflight.
Production enablement, provisioning, installation commands, release builds,
pushes and physical writes remain disabled. All changes are private test modules
and copied-workspace overlays; default kernel/boot behavior is unchanged.

## Implemented handoff

`pkg_running_root_overlay.py` selects the private enrolled boot workflow before
the first user process can seal the VFS namespace. Selection depends on the
explicit private boot command, not successful module parsing: missing/invalid
required modules therefore halt rather than selecting live authentication.
The existing measured TEST-module/selector admission still precedes NVMe writes.

`pkg_running_root.inc` reserves namespace preparation, finishes authenticated
package recovery and verifies the installed `/bin/login`, `/bin/shell`,
`/bin/sudo` and `/bin/pkg-boot` against the measured, retained bootstrap bytes.
It checks types, owners, modes and bounded maps, then fences these files and
their `/bin` parent from ordinary journaled mutation. The existing package,
ownership and account namespace fences remain active.

Clean roots enter the existing journal dirty/RECOVER lifecycle before writable
publication, even when package state is already IDLE. Actual `accounts_prepare`
uses the real detached-root fixed-role I/O and coherent database/home validator.
The isolated service instance from the previous test layer is removed; public
`SYS_ACCOUNTCTL` now serves this exact root. Account mutations stay disabled for
the immutable account-generation-v3 profile. Readiness is published only after
database validation, sync and renewed package validation.

The handoff then calls the actual `ext4_publish_root`, which calls
`vfs_boot_publish_root`, installs the running EXT4 namespace and preserves the
existing volatile `/dev`, `/run` and `/tmp` mounts. Main starts the normal
installed `/bin/login --installed` path. The test fixture includes an installed
operator account, password hash, wheel membership and owned mode-0700 home.
Bootstrap credentials are deliberately different and cannot authenticate this
installed session. The legacy opaque TEST account-profile identifier is retained;
the actual canonical account bytes are independently hashed/signed and checked.

Ordinary mutations receive device-write authority only inside the existing
semantic commit after all package/account/base-file fences and reservation
checks. Sync/freeze lifecycle authority is likewise scoped under filesystem
exclusion. No lock rank, interrupt entry contract or address-space ownership is
changed. Shutdown uses actual `storage_freeze_all`, the existing installed-root
clean marker, and NVMe controller quiesce before poweroff.

## Final evidence

Directory: `.codex-remote-attachments/pkg-boot-admission-20261024-194915-442535`.

- `running-root-manifest.json`: **8/8 boot runtime PASS** across BIOS/UEFI ×
  SMP=1/4, each with a fresh reboot of its actual shutdown output. Tests reject
  a wrong password, accept the installed password without echo, start the shell
  in `/home/operator`, use real sudo password authentication, reject unprivileged
  shadow reads, query the real global account service, and run private package
  status/acquire/recover/release against IDLE sequence 7. Base-executable writes
  are refused while ordinary persistent and volatile writes succeed.
- Every output passes Linux `e2fsck -fn`, exact persistent proof readback and
  absence of volatile proof files from disk. Superblocks are explicitly clean
  with RECOVER clear. UART logs show NVMe quiesce before poweroff.
- `installed-disk-audit.json`: independent sealed-grant readback confirms all
  eight roots remain IDLE/sequence 7; installed executable hashes match the
  measured bootstrap. GPT and unrelated sentinel bytes remain unchanged.
- `refusals/manifest.json`: **12/12 refusal boots PASS**, BIOS SMP=1 and UEFI
  SMP=4. Corrupt executable, corrupt canonical account, incoherent installed
  home, missing selector, invalid selector signature and lost account-flush
  acknowledgement all halt before root publication/login, without live fallback.
  Selector/account-evidence refusals preserve the entire image; failures after
  authenticated package recovery may retain those valid recovery writes.
  The flush-loss injection is armed only at the real account-service barrier.
- `handoff-audit.json`: private filesystem frames <=624 bytes; no new 1024-byte
  exception. Existing linked-symbol/frame gates also pass, retaining only the
  previously approved verifier ceiling. All saved compiled inputs are unchanged.

Boot/runtime acceptance and preservation are distinct. Both final full
preservation guards report **FAIL**, retained without relabeling. The positive
campaign records concurrent `src/net/wlan/wlan_mgr.c` and shared `bin/*` build
changes. The refusal campaign records only shared image/manifest changes.
Independent reconciliation verifies 1627 non-Wi-Fi source files unchanged,
including package/storage/VFS/account/process integration, with unchanged copied
inputs and module measurements. No concurrent source or shared build was staged,
reverted or rebuilt by this task outside its private copied workspace.

Exact top-level commands in WSL Ubuntu-24.04 from the repository:

```sh
python3 scripts/test_pkg_running_root.py
python3 scripts/test_pkg_running_root.py --reuse-build /mnt/c/Sources/FortressOS/.codex-remote-attachments/pkg-boot-admission-20261024-194915-442535
python3 scripts/test_pkg_running_root_refusal.py /mnt/c/Sources/FortressOS/.codex-remote-attachments/pkg-boot-admission-20261024-194915-442535
git diff --check
```

The fresh build completed compilation, measurement, symbol/frame and initial
Linux checks but stopped on its preservation assertion. Reuse verified the saved
copied inputs; an independent check verified all four module measurements before
booting. Final command nonzero exits are the retained full-preservation
assertions, not runtime failures. Exact compiler/QEMU commands, hashes, UART,
Linux output and overlay edits remain in the manifests.

## Retained development failures

All prefixes below are beneath `.codex-remote-attachments/`:

- `pkg-boot-admission-20261024-193853-966322`: probe compile FAIL because the
  power-command header was missing. No runtime acceptance; preservation PASS.
- `pkg-boot-admission-20261024-194122-878178`: installed publication/password
  login and persistent user write succeeded, then the unprivileged probe
  incorrectly tried to create a file in root-owned `/run`. Permission refusal
  was correct; the corrected root probe owns that volatile write.
- `pkg-boot-admission-20261024-194524-206611`: first installed boot and clean
  shutdown passed; reboot exposed missing writable journal activation when
  recovery was already IDLE. The handoff now follows the standard dirty/RECOVER
  lifecycle before publication. Runtime/preservation FAIL retained.
- `pkg-boot-admission-20261024-194735-435464`: snapshot guard stopped before
  overlay/build/runtime because concurrent `wlan_wpa2.c` changed during copying.
  No boot acceptance is inferred from this failed snapshot.

## Scope and remaining delivery work

This completes item 1 for a bounded, measured TEST root: recovery/lifecycle,
actual root publication, real account reader and real installed login/sudo,
fresh-boot persistence, clean freeze and controller quiesce. The running fixture
is intentionally a small system with the required login/shell/sudo/probe set;
it does not claim a complete release payload or physical-device acceptance.

The enrolled test boot protocol is separate from the legacy installer readiness
credential. Default production boot paths are unchanged. Production bootstrap
policy, incompatible-kernel exclusion, durable trust-floor advancement and
whole-disk rollback boundaries remain item 2. Generalized install/remove,
documentation members, mutable account generations and concurrent account/log
mutation campaigns remain items 3–4. Production key/provisioning/release decisions
remain item 5. No item is enabled by the TEST credentials or these results.
