# Package production integration gates

2026-10-24. Implementation plan following local checkpoint f2e162e.
This document adds no kernel ABI, boot behavior, trust material or mutation
enablement. Installation, production trust provisioning, release-media builds,
pushes and physical-device writes remain disabled. Work uses disposable fixtures.

## Accepted starting point

The [private CLI adapter](PKG2_CLI_TRANSACTION_ADAPTER.md) connects status and
recover to the bounded copied-source lifecycle. It is a process-local callback
table, not a kernel interface. The default pkg_main has no backend. Its accepted
profile is one installed demo with single-block temporary objects; arbitrary
packages, account UPDATE, upgrades and removals are outside that acceptance.
Prior host cuts do not establish actual scheduler teardown or Ring 3 delivery.

Source anchors reviewed: user/pkg/transaction_cli.h, src/kernel/syscall.h,
src/kernel/accounts.h, src/kernel/process_table.h and src/fs/ext4.h.
The existing account process-exit hook is not a package lifetime API. Retained
process-group references are group authority, not package process ownership.
ext4_root_account_namespace_admit is explicitly structural and opt-in; it does
not authenticate package records or install a durable grant. Neither API can
substitute for package mount admission.

## Gate 1: kernel backend and enrolled mount admission

First extract the accepted private validation/recovery logic into reviewed
modules without copying fixture constants, locators, certificates or host traps
into production. Keep default dispatch disabled while actual-code host tests
exercise the extracted modules. Record equivalence against the accepted vectors.

Define a bounded versioned request/result interface before assigning a syscall
number. Initial operations are status, acquire, recover and release; install,
update and remove remain unsupported. Fixed-width value fields, explicit sizes,
zero reserved fields and bounded copy-in/out are required. No user pointers,
callback addresses, device selectors, trust keys or block maps persist in a
session. Status copies a coherent authenticated snapshot before formatting.

Acquire requires effective UID zero and CAP_SYS_ADMIN on the boot-admitted
enrolled installed root. Each mutation rechecks current credentials. A retained
kernel owner identity and generation bind the session; numeric PID or descriptor
alone is insufficient. One writer is admitted. Sessions are nontransferable,
excluded from spawn inheritance and closed before exec. Releasing a handle
cannot erase a durable grant or pending operation.

Before implementation, trace concrete owner retention, descriptor close, spawn,
exec, signal exit and reaper paths. Define the final operation-reference release
that permits reuse. Cancellation prevents new work; an admitted storage step
finishes its durable outcome. Teardown publishes cancellation outside process
and scheduler locks, then drains outside filesystem exclusion. No new lock rank
or rank-1 nesting is implied. Output/copyout failure cannot retry a mutation.

Mount integration follows [durable enrollment](PKG2_DURABLE_GRANT_DESIGN.md):
independent selector authority identifies the enrolled root; read-only projected
journal validation checks trust, grants, account lineage, identities, maps and
ownership before recovery writes. Recovery and namespace fences precede account
service authentication, ordinary allocation and root publication. Partial,
foreign or unreadable enrollment refuses; absence is not an I/O error and cannot
silently select legacy writable admission. Boot-selector integrity and exclusion
of older incompatible kernels must be resolved before activation. Existing boot
behavior remains unchanged until that implementation is separately reviewed.

Exit criterion: actual extracted-code host tests pass admission, credential drop,
two competing owners, child misuse, stale identity, cancellation and fresh-owner
reuse. A private Ring 3 backend then passes status/recover and real signal/reaper
tests on disposable enrolled roots. This gate does not enable installation.

## Gate 2: public trust anchor and enrollment

Specify repository/platform identity, root and delegated catalog thresholds,
authorized public-key fingerprints, validity policy and durable version/hash
floors. Signing private keys remain outside the target OS and repository.
Fixture signing material never becomes production authority. SSH host keys and
TLS anchors are unrelated trust domains.

Implement an offline enrollment producer and a separately checked reader using
test keys first. Bind installation/filesystem/partition/enrollment identities,
resource profile, initial IDLE records and independently authenticated selector.
Prepare and read back root records before committing the selector; interrupted
cross-media enrollment must refuse rather than fall back writable. State the
whole-disk rollback and boot-administrator threat boundary explicitly.

Exit criterion: missing/partial/mixed enrollment, wrong keys/roles/platform,
expired authorization, version rollback and equal-version hash substitution
refuse before writes. Enrollment cuts preserve unrelated media. Production key
selection/provisioning remains a later explicit operational decision.

## Gate 3: bounded install and remove

Implement the original bounded FPK profile: one static executable and at most
fifteen documentation files, one application per transaction. No scripts,
services, account creation, dependencies, base replacement, self-update or boot
changes. Broader package support and upgrades are separate future milestones,
not implicit requirements for finishing this first production profile.

Derive an authoritative typed plan from verified catalog/artifact, platform/base
policy and current ownership. Reserve complete physical/accounting and bounded
memory/journal/orphan resources before intent. Publish durable trust floors
before newly authorized content. Bind every allocation/publication to the owner,
grant and immutable plan; ordinary VFS operations cannot spend package escrow.
Generalize completion evidence beyond the pre-signed demo endpoints. Remove
requires exact owned identities and content policy; it cannot delete foreign
files or lower trust/build floors. Preserve recoverable evidence until retirement
and escrow release reach IDLE.

Exit criterion: independent install/remove vectors and actual filesystem tests
cover the supported file/layout bounds, conflicts, open executable lifetimes,
low space, competing accounts/logs, every durable transition and repeated
recovery. CLI mutation verbs stay disabled until these engine paths pass.

## Gate 4: integrated acceptance and enablement review

Use isolated BIOS/UEFI x SMP=1/4 regular-file NVMe fixtures. Exercise real Ring 3
calls, concurrent admission, credential loss, SSH disconnect, signals, scheduler
teardown and reboot after storage interruptions. Recover twice in fresh boots;
check exact files, metadata, allocations, account/grant generations and IDLE.
Use Linux e2fsck/debugfs and independent record vectors. Include invalid evidence
refusals before replay and uncertain-I/O behavior without unsafe retries.

Record exact campaign hashes, faults, retained failures and tested bounds.
Report boot/runtime acceptance separately from source/media preservation.
Host fault injection and QEMU resets do not establish physical power-loss safety.
Release builds, physical writes and production provisioning remain outside this
campaign. Enabling the bounded production commands requires a final review of
these four gates; no success is inferred from compiling or CLI argument parsing.

## Immediate next implementation

Gate 1 begins with extraction/equivalence and a source-backed owner/mount call
trace. Freeze the initial status/recover request contract only after that trace;
then implement it behind private fixture admission. This is the next bounded
deliverable. No claim that the four production gates are already implemented or
accepted is made by this plan.
