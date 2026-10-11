# Persistent root Phase 3 evidence

2026-10-11. **COMPLETE — Phase 3 implementation and disposable acceptance.**
Implementation based on checkpoint `abc6625`.
Contract: [Phase 3](../plans/PERSISTENT_ROOT_PHASE3.md); prerequisites:
[Phase 0](persistent-root-phase0.md), [Phase 1](persistent-root-phase1.md),
[Phase 2](persistent-root-phase2.md).

## Scope and isolation

Serialized account snapshots and kernel-owned redo publication, `passwd`,
mandatory-password `useradd`, installed hostname/network/resolver precedence,
and private per-home history. Normal live and installer workflows remain
separate. Installer construction, target erasure, raw leases and physical
acceptance remain Phases 4/5. No physical disk writes were performed.

The runner builds a snapshot of the working tree in an isolated workspace.
Guest disks are disposable regular-file NVMe fixtures, with fresh OVMF variables
and exact-argv/path preflight inherited from Phase 2. The exhaustive host matrix
uses `/tmp/fortress-accounts-*` on the native WSL filesystem; its commands and
Linux audits are recorded in the workspace, along with the final successful and
last interrupted images. Temporary directories are removed after preservation.
Failure manifests and earlier debugging attempts are retained.

Passwords for guest tests are ephemeral runner memory. UART echo checks reject
plaintext exposure; fixtures and offline dumps contain password hashes only.
Installed guests advertise QEMU `-cpu max` for RDSEED/RDRAND. This establishes
neither physical RNG availability nor hardware entropy quality.

## Host coverage

The final complete run is `build/root-phase3/gate-w5hlde4h/manifest.json`:
**966/966 cases PASS**, comprising 872 host cases and 94 guest cases. It records
700 unchanged working-tree source hashes, commands, exact guest argv, firmware,
SMP counts, UART/stderr and Linux audits. Git HEAD remains `abc6625`; source hashes
identify the tested implementation in the working tree.

The actual redo byte engine passed 2,586 interruption/restart/corruption cases
at 49 operation boundaries under ASan/UBSan. Its labelled memory durability
adapter checks coherent old/new sets, malformed intent and failure reporting;
it does not establish mounted journal behavior.

The mounted account fixture changes all three databases, an existing password
and a new account with a private owned home. Actual EXT4/VFS/JBD2/account code
is compiled with ASan/UBSan; allocation, user-range admission and device I/O
remain labelled host adapters. It checks public versus secret snapshots,
credentials/capabilities, bounded ABI fields, writer contention and process
ownership, writer death before intent, OOM, zero-headroom ENOSPC, stale decisions,
root-only shadow metadata and denied ordinary canonical mutation or replacement.

For every actual device flush in publication, the runner cuts both before and
after that flush, rejects authentication in the failed process, then mounts in
a fresh process and performs startup recovery. Independent `debugfs` dumps must
equal the entire original or final three-file set; `e2fsck -fn` must pass.
The first device-sector write also has a failure/restart gate. Accepted sectors
may persist immediately in this file-device model. Sector tears and controller
volatile-cache orderings are not claimed by these account tests; the prior
JBD2 evidence remains separately scoped.

Both 512- and 4096-byte sector adapters passed all 215 flush boundaries, before
and after each boundary: **860 mounted failure/restart cases**. Both also passed
the write-failure gate, normal full-set publication, busy identity query and
latched authentication shutdown surviving writer cleanup. Independent Linux
three-file dumps matched the entire old or new set in every restart, with clean
`e2fsck -fn`. The two existing EXT4/VFS/ELF snapshot cases and eight boot-admission
cases also passed; the final run executed the boot, database/login, ELF page,
secure-spawn/sudo, capability, privilege and USB-mount host regressions.

Additional standalone gates passed `test-shell-host`, `test-net-ifconfig-host`
and `test-ext4-format-host`. Strict kernel and initramfs builds passed. The sudo
host fixture rejects a stale account decision with no credential change or spawn;
independent libcrypt vectors verify the actual 100000-round password generator.

## Guest coverage and completion

The focused three-boot BIOS debug fixture passed password update, mandatory
account creation, persisted hostname/network/resolver/history, new-user login,
private home ownership and sudo denial. It used an earlier positive-path kernel
and is debugging evidence, not final acceptance.

The final unchanged-source run passed **94/94 BIOS/UEFI guest cases**. Eighteen
installed runs cover three boots at SMP=1/4/8 in each firmware. They change an
existing password, create a mandatory-password non-wheel user, persist a private
owned home, hostname, network/resolver files and per-home history, then log in
with both persisted passwords. New-user sudo is denied. Offline bytes, ownership,
0600 shadow/history, 0700 home, absent pending intent, Linux crypt hashes and fsck
are independent oracles. Plaintext echo checks passed. User programs remain
BSP-pinned; these runs do not claim Ring 3 AP account-update stress.

The inherited rejection/recovery gates passed missing/degraded storage, selectors,
unsupported filesystem, readiness/manifest/flush failures, login validation and
publication failures, journal replay, multiple controllers, explicit recovery,
missing/malformed/conflicting bootstrap credentials, and authenticated recovery
across another boot. Live BIOS/UEFI boots passed absent and unusable storage.

Existing live artifacts were unchanged before/after the complete gate:

- `bin/fortress.iso`: `1fd918dff349a2928c2c56936357aaa844ae5bba84e4f92a65a5e1f0c358248c`
- `bin/fortress.img`: `cb6b774d35d4ac1cad6728179e7907efa275e2bb1c3039e5b97f26c291294119`

All QEMU children were terminated/reaped by the runner. No installer engine or
physical acceptance is claimed. Phase 4 and Phase 5 remain open.

Earlier retained attempts caught a truncated password prompt, an overly specific
offline metadata-spacing assertion, and the legacy two-argument OPEN ABI ignoring
a supplied mode argument. History now uses the existing umask and FCHMOD APIs,
restores the original umask, handles short writes and checks close/sync before
clearing dirty state. A host headroom adapter initially hit the metadata cache;
the final test bypasses that cache for the labelled free-count read only.
An exhaustive run was interrupted to move repeated host image copies from the
Windows filesystem to native WSL storage. Final review also found that writer
contention could make configuration consumers mistake installed identity for
live mode. STATUS now remains available outside the operation gate, with atomic
generation publication; host checks cover a held operation gate and disabled
authentication surviving writer cleanup. Crash loops reuse an independently
generated fixed libcrypt vector; baseline runs verify the real generator.
None of these attempts is claimed as
complete acceptance.
