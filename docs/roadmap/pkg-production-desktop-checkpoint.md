# Production PKG desktop preparation checkpoint

2026-10-28, uncommitted changes based on `main` / `0f9c7f6`.
The user approved the [enrolled-root contract proposal](../plans/PKG_PRODUCTION_DESKTOP.md).
The requested complete production workflow remains unfinished.

## Changes

Enrollment/origin and signed trust-floor readers now live in
`src/fs/pkg_enrollment.{c,h}` and `src/fs/pkg_trust_floor.{c,h}`. Their algorithm
bodies are identical to the previous accepted test sources, excluding include
names and the compilation guard. Compatibility test translation units include
the actual extracted bodies. The public policy type has its own header and
contains no keys or target identities as constants. Normal trust compilation
remains disabled by default, like the transaction engine: kernel crypto linkage
and projected admission are still required before enabling it. No boot ordering,
storage exclusions, admission rules, syscall or execution semantics changed.

`scripts/pkg_repository.py` signs the existing binary catalog role using
externally supplied Ed25519 seeds and publishes content-addressed FPK objects.
It rejects duplicate packages/signers, unsafe key files, catalog revision
rollback and equal-revision substitution. It flushes objects before atomic
catalog publication. It never generates keys or enrolls a filesystem.
The publisher's envelope/hash/manifest identity check is deliberately narrower
than the full target verifier; publication does not establish ELF/install policy.
The C interoperability test exercises full artifact verification too.

[Ubuntu instructions](../pkg-repository-ubuntu.md) describe the implemented
layout, signing/server separation, nginx example, publication ordering and
explicit cleanup of an interrupted publisher. Client configuration remains
unimplemented, so this is not yet a Dell update procedure.

Desktop documentation now records the user's supplied Wi-Fi acceptance and
correctly identifies the executor's `/usr/bin/desktop` activation path. The FPK
member stays `bin/desktop`; base `/bin/compositor` remains recovery.

## Verification actually performed

Receipt paths below are relative to `.codex-remote-attachments/`.

| Invocation | Receipt | Result and boundary |
| --- | --- | --- |
| `python3 scripts/test_pkg_enrollment.py` | `pkg-enrollment-20261028-150156-243914` | ASan/UBSan and freestanding PASS; 64 descriptor and 83 origin vectors, 3680 truncations and 3680 mutations; source/media preservation PASS |
| `python3 scripts/test_pkg_trust.py` | `pkg-trust-host-20261028-160204-788647` | ASan/UBSan and freestanding PASS; signed checkpoints, monotonic/hash binding, boot minimum/measurement/expiry/contract and corruption cases; no boot |
| `python3 scripts/test_pkg_repository.py` | `pkg-repository-20261028-145238-341517` | PASS; publication/retry, duplicate/key-mode refusals, actual C verification of 5 catalog/artifact chains and 345 truncations, signature/rollback/substitution rejection; source preservation PASS |
| `python3 scripts/test_pkg_enroll_records.py` | `pkg-enroll-records-20261028-162120-740052` | Fresh disposable GPT/EXT4 TEST records, native reader, cryptographic selector checks, 9 record-write cuts and 15 publication cuts PASS; source/media preservation PASS; no kernel boot |
| `make -j4 BIN_DIR=build/pkg-production-check/bin build/pkg-production-check/bin/fortress.elf` | local isolated output | Normal kernel link PASS, trust/executor disabled; canonical ELF/bootstrap/ISO/IMG unchanged |
| `git diff --check` | working tree | PASS |

Earlier extraction-only codec receipts also passed. They are superseded by the
guarded-code receipts above. No BIOS/UEFI x SMP=1/4 production enrollment or
desktop upgrade campaign has run in this checkpoint.

## Retained failures and qualifications

- `pkg-repository-20261028-145116-693903`: test compile failed because its
  invocation omitted src/fs and src/kernel include paths. Fixed in the harness;
  no runtime acceptance from that receipt.
- `pkg-selector-20261028-160015-662252`: compilation passed, then the old
  controller required absent historical receipt
  `pkg-kernel-backend-host-20261024-073146-531274/manifest.json`. No selector
  runtime acceptance is inferred. The fresh enrollment runner above does not
  require that archive and verifies its own fresh selector/records instead.
- `pkg-enroll-records-20261028-161416-156190`: runtime PASS, preservation FAIL
  because two documentation files changed during the run. Retained as FAIL;
  the subsequent frozen-input rerun passed both gates.

## Required remaining work

Production public-authority provisioning/reader, independent selector and
minimum delivery, projected pre-replay mount admission, kernel crypto linkage,
dynamic authenticated completion/inventory records, bounded immutable ingestion
ABI, repository configuration/download client, durable catalog/build floor
advancement and generalized UPDATE planning/activation/retirement are still
missing. The existing trust-floor boot helper still describes exact known
checkpoint offers; extracting it does not generalize pre-signed fixture endpoints.

The required installed-root matrix must demonstrate initial desktop install,
two successive distinct upgrades, exact preserved settings, reboot persistence,
invalid signature and rollback refusal, interrupted-update recovery and Linux
content/allocation/fsck oracles across BIOS/UEFI x SMP=1/4. None of those full
workflow claims follows from these host checks. Production key provisioning,
physical disk access, commits and pushes were not performed. No deployment to
the Dell is ready at this checkpoint.
