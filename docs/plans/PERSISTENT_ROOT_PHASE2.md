# Persistent root Phase 2 implementation contracts

Parent: [installer plan](PERSISTENT_ROOT_INSTALLER_PLAN.md). Builds on the
[Phase 0 policy](PERSISTENT_ROOT_PHASE0.md) and [Phase 1 mechanisms](PERSISTENT_ROOT_PHASE1.md).
Evidence: [Phase 2 report](../roadmap/persistent-root-phase2.md).

## Boot admission and namespace ownership

Absent boot selectors retain live TarFS behavior. Explicit `boot_mode=live`
also avoids legacy NVMe test initialization. Installed boot requires exactly one
`boot_mode=installed` and one canonical `root=PARTUUID=<GUID>` token. Recovery
uses `boot_mode=recovery` with the same identity binding. Duplicate, malformed,
unknown or truncated selectors fail closed. This adds no installer write lease,
installer syscall or generic installer image; those remain Phase 4.

Installed modules have exact basename/role pairs: `bootstrap.tar` /
`role=bootstrap` and `readiness.v1` / `role=readiness`. Bootstrap is bounded by
8 MiB and readiness is exactly 512 bytes. Normal admission requires both;
recovery can retain a valid bootstrap when readiness is absent. The versioned,
zero-padded readiness record binds install ID, PARTUUID, filesystem UUID,
payload version and SHA-256 of the loaded kernel and bootstrap, with a checksum
over its ordered prefix. These hashes detect incomplete deployment; they do not
provide secure boot or protect against an attacker replacing the entire image.

Before root recovery writes, require an unambiguous complete PCI inventory,
one NVMe controller, one conventional active namespace, 512-byte logical
sectors, consistent primary/backup GPT, exactly one matching Linux PARTUUID,
the fixed 256 MiB root capacity, matching filesystem UUID and successful flush.
GPT is never repaired. Unsupported EXT4 policy fails before journal recovery;
later preparation failures report that recovery writes may have occurred.

Keep PMM/VMM ordering, BSP protocol ownership and scheduler publication rules.
Prepare a detached EXT4 root before any installed process runs. Validate required
directory ownership/modes, exact versioned install manifest, complete coherent
accounts, locked root, persistent user homes and owned executable snapshots for
login, shell and set-ID sudo. Pending account redo rejects this Phase 2 gate.
Publish the root once, retaining bootstrap/module lifetime and reusing existing
devfs/runfs identities. No root swap occurs during recovery.

## Authentication and recovery

Phase 2 makes `/etc` namespace and passwd/group/shadow inodes immutable under
the filesystem exclusion. Existing readonly account consumers therefore remain
coherent. Mutation, rename, unlink and replacement reject even for root.
Phase 3 must replace this policy with its reviewed snapshot/update protocol.
Installed login uses existing `/home/<name>` directories with the user's
ownership and mode 0700; it does not create temporary runtime homes. A normal
shell exit restarts login. Login initialization or launch failure permanently
latches installed authentication off for that boot.
Collected user-shell exit codes are normalized to successful logout, so a shell
returning the supervisor's reserved failure code cannot trigger recovery.

Recovery credential and authenticator always resolve through the retained
bootstrap namespace. The independent record must be root-owned mode 0600 in a
root-owned mode 0700 readonly directory and bind the boot selector. Invalid or
missing credentials leave diagnostics and halt; they never expose a shell.
Explicit recovery and pre-admission failures do not mount the installed root.
After publication failure, authenticated recovery uses the existing active root.

The root-only console authenticator accepts 12..128 printable ASCII bytes with
echo disabled, drains overflow, handles cancellation and wipes plaintext buffers.
It verifies independent `$5$rounds=100000$` SHA-256-crypt credentials and delays
retries by at least two monotonic BSP seconds. Kernel supervisor launches the
bootstrap shell only after authentication, and reauthenticates after shell exit.
There is no default recovery secret in normal media.

## Sync, shutdown and failure states

Global storage dispatch synchronizes/finalizes the admitted root and any external
writable mount independently. Preserve readonly, retry and tainted error values.
Power action requires successful freeze/drain/clean completion followed by NVMe
quiescence. Under the filesystem exclusion, mark the root quiesced before stopping
the controller; subsequent filesystem and raw routed reads cannot submit commands.
A failed barrier or controller stop remains frozen/tainted and never reports clean
completion or reopens in place. Existing synchronous NVMe polling and DMA
quarantine remain; this checkpoint adds no physical timing/flush guarantee.

## Verification boundaries

`make test-root-phase2` creates isolated build outputs and disposable regular-file
NVMe disks only, with retained argv, source/media hashes, UART logs and Linux
byte/metadata/fsck audits. No physical disk paths, USB data disks or original live
image writes are used. `make test-root-phase2-host` runs labelled host adapters;
`--quick` / `--skip-host` are focused debugging, never the complete acceptance.
Actual installer provisioning, mutable accounts and physical acceptance remain
Phases 3, 4 and 5.
