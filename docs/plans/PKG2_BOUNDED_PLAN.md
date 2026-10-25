# Bounded install/remove typed plans

2026-10-25. This implements the read-only planning part of Gate 3. It does not
complete Gate 3, implement a filesystem executor or enable package mutations.
Installation, production provisioning, release-media builds, pushes and physical
device writes remain disabled. The accepted Gate-1/2 backend is unchanged.

## Implemented boundary

`tests/pkg_plan_candidate.{h,c}` derives a pointer-free value plan from a freshly
verified signed catalog and complete FPK artifact. It uses the actual catalog,
manifest, gzip, ELF and member-hash validators. A caller's `verified` flag is not
authority. Platform/base compatibility, a separately supplied base-command list,
current inventory identity and prior build/hash floor participate in admission.
Existing inventory/tombstone package and command identities must match the
requested package and authenticated manifest; command renames refuse.
Inputs are trusted embedding observations, not Ring-3-supplied inode identities,
free-space reports or ownership claims.

The accepted payload is one static executable and zero through fifteen
documentation files, including nested directories. Builds are not restricted to
the demo's build 1; tests use build 73. Existing incompatible installations and
upgrades refuse. A fully verified same-build/same-artifact installed package is
a no-op with unchanged sequence/generation and no reservation requirement.
Reinstallation after removal may retain its exact build/hash floor.

Remove requires a fresh authorized manifest matching the installed inventory and
the actual observed inode, generation, type, root ownership, single link, mode,
size, block count and content hash of every file. Duplicate inode observations
refuse. A name, matching content alone, or an apparent free-space refund is not
ownership. Open/pinned objects may be included in a removal plan: the future
executor must defer reclamation or account for retained orphans, re-observe
identities under filesystem exclusion, and refund only actual physical frees.
This module performs no unlink, orphan publication or refund.

The plan carries borrowed-input-independent package/command names, relative
member paths, expected metadata/content, removal identities, old-inventory hash,
catalog/artifact identity, platform and the base-name-list hash. `pi_encode`
produces an explicit 8,192-byte, zero-padded value encoding with a SHA-256 checksum;
it never serializes native padding. This encoding is not a signature. A supplied
encoding or checksum never authorizes execution; recovery must rederive the plan
from authenticated retained inputs and the admitted mount before comparing it.

Semantic failure clears candidate/encoding scratch, leaves `verified=0` and
preserves output. Invalid aliases reject without overwriting aliased input or
scratch. Consumers must check the return value, not stale scratch fields.
The context belongs in caller-owned BSS/heap, not a kernel or user stack.

## Resource requirements, not reservations

The forecast includes complete block-rounded archive and authoritative members,
a separate activation executable, full next-inventory/recovery-copy capacity,
intent, directory creation/growth and bookkeeping slack. It includes worst-case
depth-2 extent-map nodes with old/replacement nodes coexisting. Nested parent
paths are deduplicated. Staging is bounded by 32 MiB, inventory by 256 KiB and
the supplied filesystem geometry by the existing storage profile.

Admission subtracts existing reservations and preserves the independent 256 KiB
and 16-inode account floor. It checks block, inode, memory, 64-image journal and
16-orphan requirements separately. Removal does not count anticipated frees as
available resources. Failure in any dimension denies the whole plan.

These are requirements to reserve. A free-space preflight does not reserve them.
Before mutation, a kernel executor must retain memory/journal/orphan capacity,
journal physical/accounting reservations with owner identity, bind actual
allocations to the immutable plan, and enforce ordinary-writer exclusion. That
integration, a generalized durable inventory/intent format and fresh-boot
admission for these records remain unimplemented. The old private demo's fixed
state endpoints cannot safely stand in for those mechanisms.

## Verification and remaining work

`make test-pkg-plan-host` runs actual-code ASan/UBSan and freestanding checks,
with independent Python cost/encoding oracles. It does not attach a disk or
boot QEMU. [Evidence and exact limitations](../roadmap/pkg-bounded-plan.md).

Delivery still needs the filesystem reservation/execution/recovery path,
complete durable installed ownership, private install/remove CLI integration,
and installed-root acceptance with open executables, account/log concurrency,
low space and interruptions. Production enablement remains a separate review.
