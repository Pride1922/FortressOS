# Private kernel mutation binding

This extends the bounded executor with a copied-kernel transport and measured
installed-root boot profile. The default kernel, syscall table, root parser and
`/bin/pkg` remain unchanged. Only disposable regular-file NVMe images and private
test ISOs are permitted. Production provisioning, installation enablement,
release-media builds, pushes and physical writes remain disabled.

## Admission and ownership

`pkg_exec_overlay.py` composes the existing boot-module/owner extraction with
`pkg_install_overlay.py` in an isolated source copy. It restores the original
filesystem sources before applying the new executor; it does not combine two
independent semantic-commit wrappers. The kernel owns the mount and copied
catalog/artifact for its boot lifetime.

The distinct `boot_mode=pkg-exec-v1` profile requires three optical boot modules
with exact roles. A two-of-three TEST selector authenticates kernel, bootstrap,
policy and local-MAC-key fingerprint. Policy carries public selector/catalog
keys, filesystem identity, fixed ledger/namespace anchors, external ledger
minimum sequence/hash, measured base-file hashes and the immutable source.
The local MAC key enters through an external 0600 QEMU fw_cfg file, never the
policy, ISO or repository. The runner checks its absence from public artifacts.
These are fixture authorities; production key custody and firmware trust are
separate operational decisions. A boot administrator able to replace policy
and kernel remains trusted. Whole-disk rollback requires independently advanced
external minima; a MAC on the same disk is insufficient.

Admission checks unique fixture NVMe geometry, consistent GPT and selected
partition identity/type. It analyzes the journal, validates the read-only
projected ledger, ownership, account namespace and measured base files before
allowing replay. `/bin`, `/usr/bin`, `/usr/share/pkg`, `/pkg` and the legacy
transaction directory have exact expected inventories; foreign entries cannot
become trusted package executables. All measured base command names, including
`login`, are reserved. Foreign account staging/redo is rejected: this profile has an
immutable account lineage. After replay it validates again, attaches a detached
root, completes package recovery, activates the journal, prepares the actual
global account service and publishes through `ext4_publish_root`. Failure halts
the selected profile without a writable fallback. Base executables, account
files and `/bin` remain protected by the semantic commit hook. Account mutation
verbs return EROFS in this profile; status/coherent account preparation are real.

## Transport and lifetime

Private syscall 200 retains the existing status/acquire/recover/release protocol.
Private syscall 203 consumes an exact 64-byte v1 request and writes an 80-byte
result. Requests contain operation, session and inline package name, with zero
reserved fields. No user pointers, source buffers, policy/key options, device
selectors or block maps persist. The trusted registry currently contains one
slot, `toolbox`; the generic executor and CLI parser remain bounded as before.
Both input and writable output ranges are checked before copying or storage.
Operation 1 installs and operation 2 removes the registered package.

The existing `gk_transport` registers the current TCB's generation-backed
`pw_key`. ACQUIRE requires effective UID zero and CAP_SYS_ADMIN. `pw_enter`
retains an operation reference and binds the session to that owner. The new
transport holds it through `px_control` and coherent result capture, and calls
`pw_complete` exactly once before copying the result. An injected copyout error
cannot repeat or undo storage. New install/remove requests check the immutable
catalog's interval against current kernel UTC, rather than reusing the boot
clock snapshot. Boot admission still requires a currently valid source catalog;
expired-source recovery is not accepted by this fixture profile.

Status, recovery and mutation share a nonblocking backend execution credit.
Status returns EAGAIN during a mutation instead of racing with the executor's
shared verification/record workspace. Result capture occurs before releasing
that credit. It is not a spinlock and is held without filesystem exclusion
across waits; competing calls return rather than waiting on the credit.
Status also rejects a tainted engine or uncertain namespace with EIO; this
terminal-health fence has no new hardware-error-injection acceptance claim.

Between bounded steps, `mx_authorized` checks pending signals and calls
`pw_recovery_step`. That function snapshots process credentials outside its
rank-1 owner arena, then checks the retained owner/session under arena exclusion.
No process/arena lock nests with filesystem exclusion. An admitted filesystem
step completes its durable result before another authorization boundary.

Lifecycle trace: the copied `process_exit` and `thread_exit` call the existing
idempotent `gk_owner_exit` before disposal. Detach cancels the writer; operation
references prevent reuse until completion. Deferred reaping checks that the
TCB no longer owns a key. Spawn initializes a fresh TCB and does not inherit the
binding; a child cannot use its parent's session. The copied CLOEXEC sweep also
invalidates any existing binding through `pw_exec`; current spawn sweeps use
fresh unbound TCBs. No in-place exec runtime acceptance is claimed.
The owner hook ignores null/unbound keys. The existing CLOEXEC sweep still
requires a valid TCB; Ring 3 fault control checks the defensive hook alone.

## Acceptance scope

Exact frozen inputs, completed cases, retained failures and preservation
qualifications are recorded in the [acceptance audit](../roadmap/pkg-kernel-mutation-acceptance.md).

`scripts/test_pkg_exec_boot.py` builds only in a retained private source copy.
The Ring 3 probe calls the actual `pkg_mutation_cli` install/remove paths through
the new transport, plus direct protocol boundary probes. It covers competing
owners/child misuse, malformed pointers/sizes, catalog expiry without writes,
open executable byte preservation, copyout loss after a real CLI mutation,
no-op idempotence, SIGKILL with reaping and credential loss between COMMIT steps.
Both mutation and PB_RECOVER workers undergo real signal/reaper and credential
loss tests. PB_RECOVER copyout failure follows completed multistep removal;
status confirms completion, and a repeat recovery performs no additional writes.
Fresh boots recover after an intentional emulator termination at durable
COMMIT, before root/account publication, then repeat a clean boot.

BIOS/UEFI times SMP=1/4 use separate disks and paired OVMF files. Linux e2fsck
and independent file/base/ledger HMAC and floor checks audit completed outputs;
whole-disk bytes outside the target partition must remain unchanged. Runtime,
source/media preservation and intentional cuts are reported separately.

This is not full production enablement or an independent security audit.
The fixture boots the privileged probe directly from the installed root; it
does not establish interactive login/SSH mutation campaigns. Native account
writers, broader package registries, incompressible large source ingestion
(the boot policy module is capped at 32 KiB), running application execution,
fragmented-map campaigns, exhaustive sector cuts and physical power loss remain
outside this boot acceptance. The 4 MiB executable bound retains its separate
host executor evidence; this boot fixture uses the small executable plus 15
documents and the exact 64-directory bound.
