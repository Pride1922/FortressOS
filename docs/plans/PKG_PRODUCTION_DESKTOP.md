# Production desktop packages: checkpoint audit and contract proposal

2026-10-28. Source audit at clean `main`,
`0f9c7f60a4b08266818226f9b7fa07076db6471a`. This document is a proposal;
no production admission, provisioning or update acceptance is established.
User confirms Dell scan, connect, wrong-password retry, disconnect and
Ethernet/Wi-Fi handover testing complete. That confirmation is Wi-Fi evidence,
not package enrollment or update evidence.

## Implemented versus fixture authority

| Area | Actual source behavior | Missing production path |
| --- | --- | --- |
| CLI and lifecycle | Normal `/bin/pkg`, SYS_PKGCTL 95, retained owner/cancellation hooks | No normal admitted backend |
| Executor | `src/fs/pkg_engine_config.h` defaults FORTRESS_PKG_ENGINE to 0; actual engine is under src/fs | Production admission and generalized completion evidence |
| Service | `pkg_service_bind` requires an already admitted immutable backend; no production caller supplies it | Root admission before recovery/publication |
| Catalog | `user/pkg/catalog.c` verifies threshold Ed25519 signatures, repository identity, expiry and revision/hash floor supplied by caller | Provisioned authority, durable advancement and downloads |
| Artifact | Existing envelope/manifest/gzip-USTAR verifier and desktop builder | Immutable bounded ingestion into an admitted transaction |
| Install/remove | Actual planner/executor exercised by private campaigns | Catalog/artifact source and admission still supplied by private embedding |
| Upgrade | `pkg_plan.c` explicitly rejects installed build/artifact changes; identical install becomes NOOP | Atomic old/new inventory transition and recoverable retirement |
| Enrollment/floors | Signed codecs, projected validation and floor machinery exercised under tests and copied-kernel overlays | Normal selector, key delivery, independently retained minimum and old-kernel exclusion |
| Desktop | Builder emits unsigned fortress-desktop, command desktop; no mutable settings payload | Accepted repeated activation; base compositor remains recovery |

SYS_PKGCTL v1 is a values-only interface carrying a package name. It has no
source path/fd, upload, catalog refresh or UPDATE operation. Extending the CLI
alone cannot connect arbitrary downloaded bytes to the existing engine.
`px_policy.record_key` is a trusted kernel-only local MAC input; saving that key
beside its records must not be described as protection against offline rewrites
or whole-disk rollback. Existing private signed endpoints are not general
production transaction certificates.

The desktop documentation previously named `/bin/desktop` as the activated
path. The actual executor uses `/usr/bin/<command>` and protects base command
names. Documentation now identifies `/usr/bin/desktop`; the artifact member
remains `bin/desktop`. No executor path or base ownership behavior changed.

Prior passing campaigns are documented in
[the integration evidence](../roadmap/pkg-system-integration.md), with explicit
fixture boundaries. No tests were rerun for this read-only source audit.

## Protected contract discussion required before implementation

PROTECTED.md and AGENTS.md section 9 require discussion of boot dependency,
test isolation and hardware storage exclusion changes. Proposed scope:

1. Introduce explicit enrolled-root selection with independent public authority
   and minimum contract/epoch. Authenticate projected enrollment, ownership,
   trust and pending records before replay, account readiness or root publication.
   Missing/partial/mismatched required enrollment refuses; it cannot select
   legacy writable admission. Preserve unenrolled boot behavior only when absence
   of reserved enrollment artifacts is established.
2. Provision prepared metadata and read it back before publishing selection.
   Enrollment is an explicit offline operation, never an automatic boot repair.
   First implement and test only regular-file disposable images. Do not weaken
   physical storage exclusions, enable raw-write tests, or embed fixture keys,
   hardware identities, fault controls or test adapters in normal media.
3. Keep the rollback anchor independent of the enrolled root. Proposed first
   profile uses an operator-retained signed boot checkpoint, with explicit
   readback/advance after durable completion. Local floors reject release/build
   rollback while they survive. Whole-disk plus boot-policy rollback by a boot
   administrator remains outside this software-only profile. TPM/Secure Boot
   enforcement would require a separate design and implementation.
4. Preserve existing lock ranks, CPU-0 network ownership, admitted storage
   ordering and owner cancellation. Add a separately reviewed versioned ingestion
   interface without reinterpreting SYS_PKGCTL v1 reserved bytes. Kernel admission
   re-verifies catalog/artifact and derives its own plan; a userspace verified
   flag is never authority.

User approved this contract proposal on 2026-10-28, including the independently
retained signed rollback checkpoint. That approval permits implementation of
the proposed enrolled-root contract. It does not select production public
fingerprints, create production secrets or authorize physical disks.

## Reviewable implementation stages after discussion

1. Extract production enrollment/trust codecs and policy interfaces from tested
   bodies; implement independent producer/reader, projected admission and
   fail-closed selector. Prove absent/partial/mixed records and wrong-target
   rejection before writes on varied disposable geometries.
2. Add bounded immutable catalog/artifact ingestion and trusted verification,
   dynamic completion records, ownership inventory and durable floors. Retain
   current format/verifier and transaction engine; do not substitute ordinary
   filesystem copying for package activation.
3. Add strict repository configuration and bounded downloads to staging. Use
   signed catalog authenticity independently of transport. Specify exact URL
   layout, limits, expiry/clock failure, redirects and temporary-file cleanup;
   derive Ubuntu hosting/publishing instructions from the implemented producer.
4. Add UPDATE planning, peak-resource reservation, atomic activation, old-image
   retirement, cancellation and reboot recovery. Preserve `/bin/compositor`,
   `/etc/desktop.conf`, `/etc/timezone` and other mutable settings. Recovery must
   have an independently usable console/base compositor path.
5. Freeze candidate inputs and run BIOS/UEFI x SMP=1/4 installed-root campaigns:
   initial desktop install, two consecutive distinct upgrades, reboot persistence,
   exact settings hashes, invalid signatures, lower builds/catalogs, equal-version
   hash substitution and interrupted-update recovery. Validate exact bytes,
   ownership, IDLE/floors, allocations and Linux e2fsck/debugfs oracles. Retain
   failures and separate runtime evidence from source/media preservation.

The first implementation checkpoint supplies the catalog publisher and
[Ubuntu publication instructions](../pkg-repository-ubuntu.md). Client downloads,
production enrollment and upgrade activation remain outstanding. Server details
are unnecessary until the client configuration needs a concrete endpoint.
Physical deployment remains a later explicitly authorized operation after this
matrix.
