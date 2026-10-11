# Persistent root Phase 2 evidence

2026-10-11. **COMPLETE — Phase 2 implementation and disposable acceptance.**
User authorized implementation and disposable NVMe fixtures only.
Contracts: [Phase 2](../plans/PERSISTENT_ROOT_PHASE2.md),
[Phase 0](../plans/PERSISTENT_ROOT_PHASE0.md),
[Phase 1](../plans/PERSISTENT_ROOT_PHASE1.md).

## Scope and isolation

Installed boot selects explicit PARTUUID after readiness, independent recovery
credential, inventory, geometry, GPT and filesystem identity checks. Detached
root validation precedes namespace publication and persistent login. Normal
live artifacts retain their separate workflow. No installer media, raw write
lease, account update ABI, physical disk I/O or physical acceptance is claimed.

The runner snapshots the working tree into an isolated build workspace and
builds `ROOT_PHASE2_TEST=1` there. All guest data disks are fresh regular-file
NVMe copies under `build/root-phase2/gate-*`; installed guests have no source
ISO or USB data disk. UEFI uses fresh OVMF variables. Live regression uses a
separate ISO fixture with no data disk or one unusable disposable NVMe.
Preflight checks exact argv and resolved paths. Cleanup reaps each QEMU PID.
Parallel failure/cancellation stops queued cases and causes active UART waits
to unwind through QEMU termination/reaping before saving the failed manifest.
Manifests retain source hashes, existing live ISO/raw-image hashes, commands,
case results, UART/stderr and independent Linux audits. Passwords are ephemeral
runner memory; fixture archives contain hashes only. Plaintext echo is rejected.

## Executed host gates

`make test-root-boot-host` passed actual boot selector, ordered recovery/readiness
parsers, all 512 single-byte readiness corruptions, loaded image hash binding,
Limine module roles/duplicates and command-line truncation. Actual PCI parser
tests passed legacy absence, oversized/malformed MCFG ranges and overlap.
Actual recovery program passed independent Linux SHA-256-crypt vectors, empty,
short, overflow and non-ASCII rejection, no echo, cancellation wiping, monotonic
retry delays and input-flood resistance.

Host subgates passed in the final complete run, `build/root-phase2/gate-fd9ccdo5`.
Earlier standalone `make test-root-phase2-host` evidence is retained in
`build/root-phase2/gate-ewnak14n`; the final source hashes are authoritative.
Actual EXT4/VFS/ELF adapters passed 512/4096 sectors, owned snapshots,
readonly account metadata and denied replacement/mutation, 1100-node churn,
pinned executables, I/O/OOM rollback and resource reclamation. Independent
`e2fsck -fn` passed both resulting images.

Actual GPT/root policy/EXT4/JBD2 admission tests use labelled PCI/NVMe and
ELF/VMM adapters. Multiple controllers, incomplete PCI inventory, multiple
namespaces, truncated command line and failed preflight flush rejected with
zero write attempts and identical disk hashes. The actual JBD2 writer created
one committed uncheckpointed root-inode transaction; failed replay made one
failed write attempt, published no root, retained recovery and left the image
hash unchanged. Success wrote exact `/root/host-proof` bytes and passed Linux
fsck. Failed freeze tainted/froze the root and denied quiescence; successful
freeze preceded controller quiescence and prevented later routed reads.

Actual account/login, ELF page loading, spawn/set-ID/sudo helpers, capability,
power authorization and global storage dispatch regressions also passed. USB
dispatch tests are host adapters; no USB guest data disk was attached.

## Full guest acceptance

`python3 scripts/test_root_phase2.py --jobs 3` passed the complete gate, with
104 manifest cases: 94 guest cases and ten labelled host sector/admission cases.
`make test-root-phase2` invokes the same complete gate with two workers by default.
Manifest: [gate-fd9ccdo5/manifest.json](../../build/root-phase2/gate-fd9ccdo5/manifest.json).
Isolated build: `.codex-remote-attachments/ext4-phase9/guest-workspace-r0ipcjeq`.
Focused debug runs are retained separately and are not complete acceptance.
BIOS/UEFI at SMP=1/4/8 passed three boots per root with
exact persisted bytes/SHA-256/uid/gid audits, login/shell/sudo, normal shell exit
and relogin, explicit sync, clean shutdown/quiescence and abrupt-stop recovery.

Each firmware/SMP profile passed missing root, duplicate PARTUUID,
unsupported features, degraded/conflicting GPT, invalid readiness/manifest,
invalid login ELF, failed flush and committed-journal replay failure. Failures
before recovery write admission require identical whole-disk hashes. Manifest
or ELF failures after preparation may have written journal activation; the
diagnostic explicitly records this boundary. Failures never publish partial
installed namespaces or fall back to installed shadow for recovery.

Additional BIOS/UEFI cases cover login launch failure after publication, two
attached controllers with both images unchanged, explicit recovery, absent or
malformed independent credentials, independently personalized recovery records,
and live login/sudo/shutdown with absent/unusable storage.

| Guest group | Passed cases |
| --- | ---: |
| BIOS/UEFI x SMP=1/4/8 x three persistence boots | 18 |
| BIOS/UEFI x SMP=1/4/8 x ten rejection/failure profiles | 60 |
| Login launch failure after publication, SMP=4 | 2 |
| Two attached NVMe controllers, SMP=1 | 2 |
| Explicit recovery, missing/malformed credentials, independent records | 8 |
| Live boot with absent/unusable NVMe | 4 |

All source hashes remained unchanged throughout the gate. Existing live ISO and
raw-image hashes remained respectively
`d02383b9d3623f5bf02eac9a9ef9833bd698f4ef16609a339a431fe7fe3b3240` and
`cb6b774d35d4ac1cad6728179e7907efa275e2bb1c3039e5b97f26c291294119`.
Normal strict kernel/login/recovery builds passed; `git diff --check` passed.
Post-gate process inventory found no remaining QEMU or runner processes.

The first full run, `gate-ivx5xo_z`, passed all persistence boots and BIOS
rejections but exposed OVMF repairing an invalid backup GPT on writable NVMe
before kernel entry. The restored `EFI PART` signature and admitted-root UART
are retained. That run was stopped and is not complete acceptance. Its verified
runner/QEMU PIDs were terminated and a process inventory confirmed cleanup.
The final UEFI degraded-GPT fixture therefore uses `readonly=on` at the QEMU
backend. Focused `degraded-debug-c3ftu8u9` proved that the kernel then sees the
degraded GPT, rejects it, reaches credential-gated recovery and leaves the entire
disk unchanged. Other rejection and persistence fixtures remain writable.

The final account-policy audit also added protected-inode checks for removing,
moving or replacing `/etc` through its writable parent. Its actual-code host
regression passed, including moving the entire directory and replacing it with
a new directory. Installed session collection normalizes user-shell exit codes;
guest logout tests include 7 and 125, keeping the reserved login failure status
separate from a user's shell status.

## Limits

Accounts are deliberately immutable in Phase 2; mutable account snapshots,
redo and provisioning remain Phase 3. The fixture producer is host tooling,
not the Phase 4 installer. Readiness hashes are completeness checks, not a
signature/authenticated boot chain. QEMU flush/replay failure switches exist
only under `TEST_ROOT_PHASE2` and inject failing block callbacks; they do not
emulate physical controller timeout or uncertain DMA completion. PCI/NVMe host
adapters do not establish hardware enumeration, flush or power-loss behavior.
No physical writes, Dell acceptance or unrestricted multi-controller claim.
SMP guests check AP startup and the existing BSP-owned console/login path;
this gate adds no Ring 3 AP console or network access claim.
