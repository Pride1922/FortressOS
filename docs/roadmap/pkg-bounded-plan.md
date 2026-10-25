# Bounded install/remove planning evidence

2026-10-25, work based on `df28c1e`. This is the planning portion of Gate 3,
not completed package installation/removal or delivery acceptance.
[Contract and remaining integration](../plans/PKG2_BOUNDED_PLAN.md).

## Implemented and tested

`tests/pkg_plan_candidate.c` verifies catalog and artifact afresh and derives
fixed-value install, remove and verified no-op plans. It handles independent
package/command names, arbitrary authorized builds, one executable and up to
fifteen documentation files. Tests use `toolbox`/`hello` and `demo`/`demo-app`,
build 73, nested documentation and a 4 MiB executable file.

Removal checks complete observed metadata/content and exact owned identities;
modified files, different generations, unexpected ownership, links/types, block
counts and duplicate inode observations refuse. Same-build hash substitution,
lower builds, wrong platform/base, invalid/expired catalog time, bad signatures
or artifact integrity and base-command collisions refuse. A verified no-op
retains the current sequence/generation and requests no resources. Upgrades
remain excluded from the accepted scope.

The forecast includes payload, archive, activation, inventory/recovery-copy,
intent, directory and extent-tree costs. It preserves account headroom and
subtracts existing reservations; no future reclamation is counted as free space.
Tests check exact block/inode boundaries, existing reservations, memory shortage,
journal/orphan shortage, geometry/free-counter errors and counter exhaustion.
It is still a forecast: no allocator or durable reservation is connected to it.

## Exact commands and results

Run from `C:\Sources\FortressOS`:

```text
wsl -d Ubuntu-24.04 -- make test-pkg-plan-host
wsl -d Ubuntu-24.04 -- make test-pkg-verify-host test-pkg-catalog-host
```

Both PASS. Final planner evidence:
`.codex-remote-attachments/pkg-plan-20261025-070457-955227/manifest.json`.
Runtime PASS and guarded-source preservation PASS; `boot_acceptance` is explicitly
NOT RUN. Exact compiler commands, external TEST public policies/signatures,
artifacts, encoded plans, logs and source hashes are retained there. TEST signing
seeds were temporary files outside the repository and were removed after use.

| Profile | Files | Reserved block requirement (4 KiB) | Inode requirement |
| --- | ---: | ---: | ---: |
| `toolbox`, command `hello` | 1 | 147 | 7 |
| Fifteen documentation files | 16 | 228 | 40 |
| Nested documentation, 4 MiB executable | 16 | 2293 | 41 |
| 4 MiB executable only | 1 | 2210 | 7 |

Each profile runs actual-code ASan/UBSan with fragmented reads, positive
install/remove/no-op cases and adversarial resource/identity/policy cases.
Independent Python checks the canonical plan hash, package/command identities,
file count, deduplicated parents and full block/inode requirements. Additional
C cases cover 1 KiB/2 KiB block-size forecasts. The freestanding strict build
passes with the largest planner frame **496 bytes**, encoder **112 bytes**.
These frame checks do not establish complete Ring-3 call-chain stack margins.

Existing validation regressions PASS:

* Envelope/identity structural checks and 234 independent manifest vectors.
* 82 artifacts × four fragmentation profiles, 490 truncations and 491 read
  failures; existing verifier context remains 19,808 bytes.
* 40 signed catalog vectors, 345 truncations and six complete artifact chains.

`git diff --check` also passes. No ISO, installed-root boot, device write or
production trust provisioning was performed by these tests.

## Retained failures and intermediate runs

The first attempt,
`.codex-remote-attachments/pkg-plan-20261025-064127-350644`, has runtime FAIL and
preservation PASS. The harness omitted filesystem/kernel include paths, selected
the system ELF header instead of Fortress loader constants, and could not find
`vfs.h`. The include paths were corrected; the failed logs remain unchanged.

Intermediate attempts `064251-252848`, `064907-857654`, `065132-485779` and
`065452-719125` pass runtime and preservation for their recorded source versions.
The `070006-321353` run added independent package/command names. The final run
also binds inventory/tombstone package and command identity to the request and
manifest, refusing command renames and a foreign package's inventory view. All
corresponding source hashes are retained. None is relabeled boot acceptance.

The original production integration plan remains unchanged:
`f1e8a769becaac55509a82a1633fda9c6057167588becab8c9cba91521fb1017`.
Concurrent Wi-Fi commits are retained; this batch does not modify their code.

## Delivery is still incomplete

No filesystem mutation executor consumes these plans yet. Actual installed-root
observation must be obtained under the admitted mount's exclusion, generalized
inventory/intent records must be authenticated on projected recovery, and full
resource reservations must be durably connected to allocation and ownership
transfer. Current package records still admit only the earlier demo endpoints.
The planner's trusted inventory/observation inputs cannot be filled from an
untrusted request or by adopting whatever happens to occupy a pathname.

Pins are deliberately not an authorization denial in a read-only plan. Runtime
deletion/refund must defer safely for open executables and prove actual bitmap
frees; those lifetime cases are not tested by this new planner suite. The supplied
catalog must be valid under the trusted policy time; historical removal-policy
selection is not implemented by this module. Physical extent/parent admission,
bounded staging and cleanup, fresh-boot recovery, private install/remove CLI
operations and combined account/log/low-space interruption campaigns remain.
Installation and production enablement stay disabled until those paths pass.
