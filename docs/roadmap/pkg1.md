# PKG-1 design and structural validation checkpoint

2026-10-21. [Design and pending decisions](../plans/PKG1_DESIGN.md).

Reviewed initial HEAD `199b575` and preserved unrelated logging/Watchtower work.
During this task HEAD advanced concurrently to `67947b2` (logging timestamp
commit); this task made no commit and did not restore those files. New work is
limited to package design, a pure envelope/identity validator, host tests and its
Makefile target. No production runtime integration or extraction is enabled.

Executed from PowerShell in C:\Sources\FortressOS:

```
wsl -d Ubuntu-24.04 -- make test-pkg-format-host
git diff --check
```

Both PASS. Actual validator code built with GCC C11 strict warnings under
ASan/UBSan and ran: valid minimal envelope, every short header length, oversized
header, magic/version/header-size/flags/reserved rejection, zero/max/+1 manifest
and payload limits, uint64 maximum lengths, NULL arguments, unchanged output on
failure, malformed/maximum identity components. Sanitizers reported no errors.
Separate freestanding/no-red-zone/no-SIMD compilation passed. GCC stack-usage
records in `build/pkg-format/format.su`: parser 88 bytes, identity 48, le64 32,
le32 24. These are compiler-reported local usage, not Ring 3 full-chain proof.

No gzip/USTAR, ELF, signature, trust-floor, state database, lease,
reservation or recovery implementation is claimed. Candidate manifest/catalog
formats need independent vectors before freezing. This gate does not prove
arbitrary stream parsing or package authenticity. Installed NVMe BIOS/UEFI,
interruption/recovery and Linux audits are NOT RUN for PKG-1: there is no target
mutation component to exercise yet. Their concrete future matrix is in the
design; existing EH storage evidence does not count as a PKG pass.

No release image, physical disk writes, commit or push. ABI, account recovery
floor, locks, boot ordering and physical-write gates are unchanged. EH-1B remains
separate. Writer lifetime, competing allocation reservation, trusted platform
identity/provisioning, signing custodians/expiry and recovery integration remain
explicit input/review gates before enabling installation.

## Continuation: bounded manifest parser

The user's continuation authorizes progressing independent validation work;
signing/provisioning and kernel lease/reservation decisions remain unresolved.
Added `user/pkg/manifest.c`, borrowed-view API in `format.h`, independent Python
wire producer `scripts/pkg_manifest_vectors.py` and sanitizer host harness.
Parsing performs a validation pass before publishing output, with caller-owned
immutable input, no allocation, I/O or large output temporary on the stack.
Failure preserves the entire output. Successful views borrow the input lifetime.

Re-ran `wsl -d Ubuntu-24.04 -- make test-pkg-format-host`: envelope PASS and
**234 independent manifest vectors PASS**, plus 10,000 deterministic random
malformed-input stress iterations under ASan/UBSan. Stress is memory-safety and
output-preservation coverage, not an independent validity oracle. Python vectors
cover every truncation of a valid two-record manifest, bad magic/version/count,
range/zero-build/platform-ID/text failures, paths, duplicate/order/ancestor
conflicts, role/mode/reserved errors, executable size and 16/+1 file limits,
expanded tar framing budget and uint64 maximum size rejection. Accepted views
also verify package/build/base fields, first-file mode/hash and computed archive
length. Strict freestanding compilation PASS; largest compiler-reported local
frame is `parse` at 576 bytes, entry 48, helpers <=64. No Ring 3 stack claim.

Format reconciliation: relative paths now cap at 100 bytes, matching USTAR's
name field while prefix fields are forbidden; exact 100 and 101 byte vectors
pass/reject respectively. Zero platform ID rejects. Files cannot be ancestors
of other listed files, including when a lexicographically intervening sibling
separates the ancestor/child records. Candidate grammar is documented, not a
release compatibility promise. No hashes/signatures/payload/ELF verification or
destination/base ownership checks are implemented by these syntax parsers.

## Read-only complete-artifact verification

Added `user/pkg/verify.h` / `verify.c`, `scripts/pkg_artifact_vectors.py`,
`tests/pkg_verify_host.c` and `make test-pkg-verify-host`. Existing SHA-256 and
gzip code are reused through an additive serialized gzip callback adapter.
Invalid callback I/O is latched as failure. No extraction/file-writing interface;
the host syscall stub asserts on any unexpected CLI syscall.

Executed:

```
wsl -d Ubuntu-24.04 -- make test-pkg-verify-host
wsl -d Ubuntu-24.04 -- timeout 60s make test-gzip-host
git diff --check
```

PASS: envelope/identity checks, 234 manifest vectors, 10,000 malformed manifest
stress iterations; **82 artifact vectors x four fragmentation patterns** (1,
17, 4096 bytes and deterministic varying prefixes), **490 bytewise truncations
and 491 injected read-error positions**, bad external hash and overreporting
callback. ASan/UBSan clean. Gzip regression PASS including Linux interoperability
and existing 100 MiB bomb-cap tests.

Artifact cases include stored/fixed/dynamic DEFLATE, 32 DEFLATE mutations with
Python gzip oracle, CRC/ISIZE, flags, concatenated members/suffix, truncation,
reserved blocks, expansion overrun, tar hash/checksum/padding/path/type/link/
prefix/uname/end-marker errors and outer hash/length failures. Malformed inner
fixtures have recomputed outer hashes so tests reach inner validation. ELF
negatives cover BSS over mapped limit, W^X, entry, zero/upper-half/congruence/
reserved-page addresses, dynamic/interpreter/TLS segments, program-header count/
offset, file bounds, overlap and short files. Exact 4 MiB mapped memory accepts;
+1 rejects. ELF is not executed.

Python standard-library tarfile independently inspects fixture names and exact
executable bytes; gzip independently expands the archive. Two producer calls
compare byte-for-byte in the same Python/zlib environment. Synthetic unsigned
fixture `build/pkg-format/fixture.fpk` SHA-256:
`db2ac351543a6f7afc6203c3d72b3478985b0d418c7677c754acd59c501a750b`.
Case labels/expectations are in `build/pkg-format/artifact-vectors.json`, with
binary vectors alongside. This is a fixture builder, not a release packager,
signer or cross-toolchain reproducibility claim.

Retained failure: the first artifact run hung in existing gzip `decomp_get_bits`
when EOF left positive residual bits fewer than requested. Refill could not
progress. That process was terminated. The fix rejects insufficient bits
immediately; the final bytewise truncation/read-fault campaign passes under a
60-second timeout. Earlier partial run is not PASS. No fixture contract was
relaxed to avoid the failure.

Freestanding verifier/gzip/digest builds with no red zone/SIMD and 1024-byte frame
warning-as-error PASS. Maximum verifier local frame 168 bytes; gzip decoder 256
(whole module maximum 480); SHA block 376; manifest 576. Context 19,808 bytes must
reside outside user stack. Gzip uses its existing static workspace, serialized
only. Local figures do not prove nested Ring 3/signal stack safety. The loader
maps 16 KiB user stack, with argument-table budget in top 4 KiB; earlier package
plan total-stack wording was corrected.

Integrity success is not signature authentication, platform/base compatibility,
destination ownership or install authorization. Signing/trust floors, catalog,
live CLI, Ring 3, extraction/mutation, installed NVMe, Linux filesystem audits
and recovery acceptance remain NOT RUN or unimplemented. Host tar/gzip checks
do not substitute for later filesystem audits. No release image, commit, push
or physical writes. Concurrent AGENTS.md encoding edits appeared and were left
untouched. HEAD remains `67947b2`; EH-1B stays separate.

## Candidate signed-catalog verification

Added `user/pkg/catalog.h` / `catalog.c`, independent Python/OpenSSL signed
vectors, sanitizer harness and isolated build runner. Candidate wire/policy and
limitations: [PKG1_CATALOG_DESIGN.md](../plans/PKG1_CATALOG_DESIGN.md).
No production signing keys are generated or selected. The three deterministic
test seeds/public keys are deliberately public fixtures. No signing API or
private-key operation is added to the client.

Executed `wsl -d Ubuntu-24.04 -- make test-pkg-catalog-host`: PASS, including
the existing format/artifact dependency gates, **40 catalog vectors, all 345
truncation prefixes and six catalog-to-artifact checks**, ASan/UBSan clean.
The six composition checks cover valid 2-of-3/3-of-3 catalogs, identical current
revision, artifact build/name mismatch and wrong artifact digest. Catalog cases
include threshold failure, invalid policy/keys, duplicate/unknown signers,
tampered/bad signatures, signature order, wrong repository/root/role, expiry/
future issue/invalid clock, revision rollback, same-revision different object,
reserved/extra bytes, entry/name/build/length policy, 256-entry boundary and
257-entry rejection. Rejection preserves catalog output; composed artifact
failure clears the verification flag.

Python cryptography 41.0.7 independently signs/verifies canonical message bytes;
actual Fortress crypto wrappers and namespaced pinned Monocypher sources verify
them in C. Objects compile only in `build/pkg-format`; shared crypto library,
boot/kernel/image targets and upstream sources are untouched. Runner bounds each
subprocess at 60 seconds. Case expectations/public keys/catalog identity are
recorded in `build/pkg-format/catalog-vectors.json`, binary vectors adjacent.
Freestanding catalog compile with <=1024-byte frame gate PASS: verifier local
224 bytes, composition 160, lookup 80, helpers <=64. This is not a full Ring 3
call-chain proof; the existing documented upstream verifier frame exception
remains unchanged. Crypto primitives were reused, not modified.

This advances authentication only against the supplied pinned catalog-role
policy. It does not implement root delegation/rotation, key provisioning,
trusted time acquisition, durable revision publication, installed build floors,
platform identity/base compatibility, downloads, installation, lease/reservation
or recovery. Higher-revision acceptance in memory never implies a persisted
floor. All target mutation remains disabled. No release image, physical writes,
commit or push. Concurrent unrelated AGENTS.md edits remain untouched.

## Read-only CLI and transaction-contract review

2026-10-21. Initial HEAD for this continuation is `e360429`; the concurrent ETCFS
implementation/AGENTS changes were committed independently and preserved.
Reviewed current headers, verifier/control flow and filesystem account floor.
Added actual freestanding `user/pkg/cli.c` / `cli.h`, mock syscall tests,
isolated CLI build runner and `test-pkg-cli-host`. No default build/image rule
installs the CLI. Existing kernel ABI and storage behavior remain unchanged.

Commands:

```
pkg --help
pkg inspect ARTIFACT BYTES SHA256
pkg verify CATALOG PACKAGE ARTIFACT
```

Inspect requires caller-supplied decimal exact length and lowercase SHA-256;
output explicitly states signatures are not checked. Verify requires an embedded
policy, replaces its clock snapshot with SYS_CLOCK_GETTIME UTC and rejects missing
or invalid time. Default `pkg_main` supplies no policy and fails closed rather
than trusting command-line keys, environment data or test keys. Authenticates
catalog and finds package before opening artifact. It reports supplied catalog
policy verification, not running-platform compatibility or installation approval.
Future production provisioning remains separate. Both paths open O_RDONLY with
O_NOFOLLOW/O_NONBLOCK, require retained-descriptor FSTAT regular-file type and
bounds, accept fragmented reads, close descriptors on every branch and suppress
success on verification/close/output failures. Writes are only to stdout/stderr.

Executed `wsl -d Ubuntu-24.04 -- make test-pkg-cli-host`: PASS, including prior
format/artifact/catalog dependency gates. Actual CLI/core/crypto code under
ASan/UBSan with **mocked syscalls** checks help/usage, integrity and supplied-policy
success, missing policy, bad signature/artifact, read errors, close failure,
nonregular metadata, invalid clock, overflow length, short hash, unsupported
install and failed output. Mock transport returns 5-byte reads and 7-byte writes,
asserts read-only open flags, permits no file-write operation and checks zero
leaked descriptors after each invocation. This is not kernel syscall acceptance.

Private freestanding build/link PASS: `build/pkg-cli/pkg.elf`, 43,520 bytes,
89 mapped pages, no unresolved symbols or W+X segment, no dynamic/interpreter
segment. ELF SHA-256:
`d7d1d8cf10272c055ccf37da215b6b8f61953753cdb874b9d57abfa1efbc40ac`.
Caller-owned context/catalog buffers live in BSS. Runner compiles package frames
with <=1024-byte ceiling, records `.su` files and asserts exact source hashes
unchanged during the gate in `build/pkg-cli/manifest.json`. Crypto upstream is
unchanged and retains its previously documented verifier frame exception.
Private ELF is not packaged or executed: **Ring 3 and BIOS/UEFI are NOT RUN**.
No full nested-stack/signal or target usability claim.

Added [transaction review](../plans/PKG2_TRANSACTION_REVIEW.md): owner-lifetime,
nontransferable lease-bound session dispatch, fd/reaper/exclusion requirements,
allocation ledger/account priority, candidate durable record header/inventory
layout and interruption acceptance matrix. It identifies a blocking gap:
volatile reservations do not survive death/reboot. Manual recovery plus a
package-only lease cannot guarantee free space before competing logs allocate.
Durable mount-admitted reservation or proved allocation-free post-intent recovery
needs separate review. No kernel implementation, syscall number or protected
boot/lock/ownership contract change was made. Pending-state safe failure is not
an unconditional recovery-progress guarantee.

Remaining: provisioned trust/platform identity, durable floors/inventory/intent,
final pending grammar, session/reservation APIs, kernel implementation and
actual installed NVMe interruption/recovery plus Linux audits. No release image,
commit, push, physical writes or EH-1B timestamp policy changes.

## Private read-only CLI Ring 3 validation after b0693ee

2026-10-21. Read PROTECTED.md, AGENTS.md and all three package design/review
documents before continuation. Reviewed actual CLI, common I/O helpers, syscall
dispatch, VFS open validation and loader/header contracts. Found a real API
mismatch: CLI and its syscall mock accepted O_NONBLOCK on ordinary SYS_OPEN,
but VFS open_common rejects that flag. Corrected only CLI read-only open flags
and the mock assertion; O_RDONLY | O_NOFOLLOW plus retained SYS_FSTAT checks
remain. No kernel ABI or storage-policy change. Historical O_NONBLOCK wording
above describes the checkpoint implementation, not the corrected behavior.

Added `scripts/test_pkg_cli_qemu.py` and `make test-pkg-cli-qemu`. The runner
copies sources/dependencies, runs host gates, links the ordinary no-policy CLI
and a separate fixture entry with public deterministic test keys, then builds
only a private initramfs/ISO. Nothing stages pkg in the default build. Test
catalog signatures are independently generated by Python cryptography/OpenSSL,
with broad fixture validity dates; CLI uses actual kernel UTC, not policy.now.
QEMU argv is checked on every boot: optical fixture and paired OVMF pflash only,
no NIC or data disk. UART waits and subprocesses are bounded; QEMU is terminated
on every failure and cleanly shut down on success.

Executed:

```
wsl -d Ubuntu-24.04 -- make test-pkg-cli-host
wsl -d Ubuntu-24.04 -- python3 scripts/test_pkg_cli_qemu.py
wsl -d Ubuntu-24.04 -- python3 scripts/test_pkg_cli_qemu.py --replay .codex-remote-attachments/pkg-cli-qemu-20261021-080111
git diff --check
```

Host gates PASS, including rerun against the corrected CLI in the copied
workspace: envelope, 234 manifest vectors, 82 artifacts x4 fragmentation,
490 truncations/491 read errors, 40 catalogs/345 prefixes/six compositions,
ASan/UBSan CLI mock failures and private freestanding link/frame checks.
Actual Ring 3 **4/4 boots PASS**, BIOS/UEFI x SMP=1/4, ten checks each with
expected output AND exit status: help, valid integrity inspection, default
missing-trust rejection, embedded-test-policy authenticated verification, bad
signature, absent package, bad artifact under both commands, nonregular input,
unsupported installation. Integrity success names demo/build 1; authenticated
success still explicitly excludes platform/install admission. No fixture
executable is installed or run from the package payload.

Evidence: `.codex-remote-attachments/pkg-cli-qemu-20261021-081925/` contains
per-boot argv/serial/stderr/result files and the aggregated summary/manifest.
Prepared binaries, generated test-policy C, host/frame/build logs and private
media remain under `pkg-cli-qemu-20261021-080111/workspace/` and its build.log.
Default CLI SHA-256 `a7260733577e658f3681e32f86a4788a78c9f804547ec939a3abe181986bf549`;
test-policy CLI `36baf0e32535f66a3c10add79de5ddecab6ab1cb41c7bc110af3b99a4ddd362a`.

**Preservation guard FAIL, qualified separately from boot passes.** HEAD advanced
concurrently from b0693ee to `497e201` (network resolv.conf relocation). Source
hash differences include those networking files and our runner/Makefile edits.
Shared bin/fortress.img, fortress.iso, fortress.manifest.json and initramfs.tar
changed concurrently; exact before/after hashes are retained. This runner built
only in its copied workspace and did not restore or overwrite shared artifacts.
The original final assertion failure is retained; summary/manifest were aggregated
afterward from the four per-case PASS records. No unconditional source/media
preservation PASS is claimed. Final runner saves both results and differences
before failing this guard on future runs.

Retained earlier failures: 075442 copy guard included a nested build marker that
copy exclusions omitted; fixed source-inventory filtering. 075550 ran the old
CLI and rejected valid inspection, consistent with the unsupported flag.
075659 reached three boot passes then exposed an echoed-command status-match
race; output-line matching fixed it. Its temporary WSL workspace disappeared
before replay; later workspaces are retained alongside evidence. 080111's
first boot timed out because the stricter matcher omitted the help-line prefix;
the corrected replay completed all four boots. Failed/partial runs are not
reported as full passes. No acceptance assertion was dropped.

Scope remains functional read-only CLI acceptance on immutable initramfs files.
No installed-root/NVMe, filesystem recovery/Linux audit, physical acceptance,
full nested-stack watermark, maximum argument or signal-stack margin claim.
Installation, production trust provisioning, durable floors, lease/reservation
and recovery remain disabled/unimplemented. No release-media build by this task,
physical writes, commit or push; concurrent work was left intact.

## Private nested-stack, maximum-argument and signal margins

2026-10-21. Continued the uncommitted checkpoint at HEAD
`497e2011746731538c7b40c14e6d92e27c387ba7`. Read PROTECTED.md, AGENTS.md,
all three PKG design/review documents and this roadmap. Preserved the existing
CLI/API fix, runner, tests and documentation changes. Added only private stack
fixtures (`tests/pkg_stack_ring3.c`, `scripts/test_pkg_stack_qemu.py`), an opt-in
`make test-pkg-stack-qemu` target and additive runner support/reporting. No
production kernel, loader, signal ABI, package CLI or installation behavior was
changed in this continuation.

Executed:

```
wsl -d Ubuntu-24.04 -- python3 scripts/test_pkg_stack_qemu.py
git diff --check
```

Final campaign: `.codex-remote-attachments/pkg-cli-qemu-20261021-102106/`.
**Boot acceptance PASS, 4/4: BIOS/UEFI x SMP=1/4.** Every boot checks the ten
existing CLI cases and two stack probe commands, including their exit statuses,
then shuts down cleanly. Each stack command spawns an ordinary Ring 3 child with
32 arguments AND 32 environment entries, exactly 1,024 bytes per vector including
terminators, with 256-byte maximum individual strings including NUL. The child
checks these counts/byte totals. The maximum loader footprint remains on its
stack while the wrapper calls the real CLI with its supported five arguments;
the CLI itself does not accept 32 arguments. Aggregate, string-length and count
+1 variants return E2BIG for both argv and envp. These rejection vectors can
also exceed the aggregate budget; they do not independently isolate each
kernel rejection reason.

Both probes run successful supplied-test-policy verification of the original
artifact and independently generated dynamic/fixed/stored DEFLATE variants,
plus bad-signature rejection. Python zlib block-type assertions and gzip decode
checks confirm the three variant payloads. No payload executable is launched.
Only public deterministic fixture signing keys are used; production trust is
still absent. The same host sanitizer, format, artifact, catalog and CLI gates
run successfully in the copied workspace before its kernel/private media build.
Package-local compiler frame gate remains <=1,024 bytes; upstream crypto keeps
its existing separately documented exception, with no source modification.

All four boots produced identical minimum remaining margins above
USER_STACK_BOTTOM_VIRT:

| Measurement | Bytes remaining |
| --- | ---: |
| Ordinary code, untouched-prefix stack watermark | 11,240 |
| Instrumented full call chain, sampled RSP | 11,160 |
| Instrumented call chain plus signal handler, RSP and watermark | 10,408 |

The probes fill only below their current active frame, then scan for the first
changed byte before printing results. An untouched-prefix watermark measures
writes; unused frame space need not change it. The instrumented executable
compiles every package, common I/O, gzip, digest, string and crypto C unit with
`-finstrument-functions`, sampling allocated frames through enter hooks.
Instrumentation changes code shape/overhead, so the sampled figures describe
that exact executable, not the uninstrumented binary's exact minimum RSP.
The plain probe's `sampled_margin=13616` is its wrapper starting RSP only; its
useful nested measurement is the 11,240-byte watermark.

The instrumented second pass sends SIGINT to its own retained numeric PID at
every newly observed stack low. **34 deliveries/returns per boot** complete
while nested verifier calls remain active. The separate uninstrumented handler
touches and verifies 512 volatile scratch bytes (compiler stack usage 520 bytes),
records its RSP and returns through the actual kernel signal frame/restorer.
Both verification results and rejection results remain correct afterward.
The signal margin includes the kernel's 224-byte frame, return slot/alignment,
hook overhead and handler frame. A 4,096-byte minimum remaining margin is asserted
by each probe. No reentrant verifier call or nested-signal claim; arbitrary
larger application handlers are outside this fixture.

Evidence includes per-boot argv, UART, stderr and result JSON, aggregate
`summary.json`, source/media `manifest.json`, private copied workspace, build
log, both probe ELFs and compiler `.su` files in `workspace/build/pkg-stack/`.
The argv checks admit only private optical media and paired OVMF pflash, no NIC
or data disk. Waits are bounded and QEMU cleanup runs on failure as well as
success. Measurements cover these compiler flags, synthetic artifacts and
branches; they do not prove a universal adversarial-input maximum, different
toolchains, arbitrary signal nesting or installed-root behavior.

**Preservation checks PASS, reported separately from boots.** The final
manifest reports `source_preservation=PASS`, `media_preservation=PASS` and empty
change lists for the inventoried source bytes and pre-existing shared `bin/`
files. HEAD remained `497e201`. This is the runner's explicit inventory scope,
not a claim that the entire repository contains no uncommitted work. Roadmap
and design updates following the gate are intentional. No shared release media
was rebuilt/restored; all media builds were private fixture builds.
Unrelated concurrent AGENTS.md and README.md changes were visible by the final
status check and left untouched; those documentation files are outside the
runner's source-byte inventory.

Retained partial failures: `pkg-cli-qemu-20261021-101621/` reached the first
instrumented BIOS/SMP=1 probe (11,176-byte sampled nested margin, 10,424-byte
signal margin, 34 signals), then the harness's generic negative-output assertion
incorrectly rejected its expected authenticated-success output. The already
started `101855/` campaign hit the same assertion after the plain BIOS/SMP=1
probe. Both retain serial/failure logs and are NOT full passes. Restricting that
negative-output assertion to ordinary rejection cases fixes the harness; stack
thresholds, results and exit-status assertions were not relaxed. The final
`102106` campaign rebuilds and passes the expanded fixtures. Initial sandboxed
WSL invocation was denied before execution; the approved WSL run supplied the
evidence above.

Installation, release integration, production trust/platform provisioning,
durable floors, writer lease/reservation, transaction/recovery and installed
NVMe acceptance remain disabled or unimplemented. No release-media build,
physical disk write, commit or push was performed. Existing concurrent work was
preserved; no physical acceptance or filesystem-recovery claim is added.

## Isolated maximum-vector rejection and stack review

2026-10-21. Continued the checkpoint at HEAD
`a6cdffa0af271d5547f4321dfd0c68eb484d73e3`; prior concurrent work, including
AGENTS.md and README.md, was already present and was left intact. Reviewed the
completed `pkg-cli-qemu-20261021-102106` summary, preservation manifest, probe,
build recipe, harness and actual spawn-vector/signal-frame implementation.
Its four boot passes and separate preservation passes remain valid in their
documented scope; overlapping rejection limits were a coverage limitation.

Replaced those rejection fixtures with six independent cases, each checking
both vectors' count/string/total violation masks before SYS_SPAWN_EXT:

| Limit tested, separately for argv and envp | Tested vector including NUL | Other vector |
| --- | --- | --- |
| Count +1 | 33 empty strings: 33 bytes, maximum string 1 byte | One empty string |
| Individual string +1 | One string: 257 bytes total | One empty string |
| Aggregate bytes +1 | Five strings: 256 + 256 + 256 + 256 + 1 = 1,025 bytes | One empty string |

Each case requires E2BIG; unexpected successful children are killed/reaped before
failure. The existing accepted child still retains 32 argv and 32 envp entries,
1,024 bytes each, with 256-byte individual strings. Child checks now bound each
string, check the argv terminator and bound envp traversal. The wrapper invokes
the real CLI using five supported arguments while the maximum loader footprint
remains resident; this does not claim that pkg accepts 32 CLI arguments.

Review fixes: the supplied-test-policy wrapper was previously linked without
instrumentation, leaving a gap in the claimed C call-chain sampling. It is now
compiled with -finstrument-functions for the instrumented probe (48-byte .su
frame). Hook and handler remain deliberately uninstrumented; handler .su frame
is 520 bytes. Added an explicit >=4,096-byte handler-RSP floor. The harness now
requires all six individual rejection markers and complete per-command metrics,
checks margin ranges and signal counts, and records metrics separately for each
probe in result.json. Prior combined stack_metrics remain available.

Executed `wsl -d Ubuntu-24.04 -- python3 scripts/test_pkg_stack_qemu.py` and
`git diff --check`: PASS. Evidence is
`.codex-remote-attachments/pkg-cli-qemu-20261021-105323/`, including retained
copied sources, build.log, private media/ELFs/.su, per-boot argv/UART/results,
summary.json and manifest.json. Copied-workspace ASan/UBSan gates PASS:
envelope/identity, 234 manifests, 82 artifacts x4 fragmentation, 490 truncations,
491 read failures, 40 catalogs, 345 catalog truncations, six composed artifact
checks, and CLI mocked-syscall success/failure/cleanup checks. Private strict
freestanding builds and package-local <=1,024-byte frame gates PASS; upstream
crypto and its existing frame exception are unchanged.

**Boot acceptance PASS: BIOS/UEFI x SMP=1/4, 4/4.** Every boot passes the ten
ordinary CLI cases and both stack commands with expected exit statuses, six
isolated E2BIG cases per probe (48 across the matrix), and clean shutdown.
All four boots have identical margins above USER_STACK_BOTTOM_VIRT:

| Measurement | Bytes remaining |
| --- | ---: |
| Plain nested untouched-prefix watermark | 11,224 |
| Plain wrapper-only sampled RSP (not nested RSP) | 13,600 |
| Instrumented nested watermark and sampled RSP | 11,112 |
| Instrumented signal watermark and handler RSP | 10,360 |

Each instrumented boot completes 35 signal deliveries/returns during new sampled
lows. The successful original and dynamic/fixed/stored artifact verifications
and bad-signature rejection remain correct. The changed code shape and added
wrapper instrumentation change margins and delivery count relative to 102106;
these are measurements of these exact ELFs, not a stack regression comparison.

**Preservation PASS separately:** manifest source_preservation=PASS and
media_preservation=PASS, empty source/media change lists, HEAD unchanged. This
covers the runner's inventoried src/user/tests/scripts/Makefile bytes and
pre-existing shared bin files, not all repository files. This roadmap addition
follows the completed check. No shared release media was built or restored.

No new failed or partial campaign in this continuation. Earlier 101621/101855
harness failures and all earlier failures remain retained, with their original
qualifications above; nothing is reclassified as a full pass. Function-entry
sampling does not sample every instruction or assembly helper; watermarking
observes writes rather than untouched allocated frame bytes. Instrumentation
changes frame/code shape. Synthetic payloads/branches and one 512-byte handler
do not prove arbitrary adversarial-input maxima, reentrant verification,
nested signals, other toolchains or installed-root/filesystem recovery.
Only public fixture keys are used. Installation and production trust provisioning
remain disabled; no kernel behavior, release integration, physical disk write,
commit, push or physical acceptance was added.

## Recovery-space design audit

2026-10-21. Added [PKG2_RECOVERY_SPACE_REVIEW.md](../plans/PKG2_RECOVERY_SPACE_REVIEW.md)
and linked it from the transaction review. Static audit at HEAD `a6cdffa` traces
mounted rename/replace, directory insertion, file extent rebuilding, orphan
cleanup, journal staging, ordinary/account allocation checks and detached-root
account preparation. Existing uncommitted stack fixtures and documentation were
preserved; AGENTS.md and README.md were not edited.

Finding: the current complete protocol cannot claim allocation-free post-intent
recovery. Absent-name publication may grow the destination directory; file
rewrites/reconstruction can allocate external extent nodes even with existing
data blocks. Existing-file replacement and right-edge orphan cleanup avoid
filesystem allocation but still need volatile cache/heap/journal/orphan resources.
Historical EH-2 zero-free-block evidence supports those narrower operations.

Recommendation: develop a persistent mount-admitted recovery grant plus prepared
publication objects. Atomic grant spending with allocation, admission before
allocating account preparation, protected namespace/record identity and account
priority are required design gates. Existing account callbacks bypass the ordinary
floor, so adding only an ordinary package floor cannot promise progress. No
account behavior, boot ordering, allocator, syscall or package operation changed.
Authorization expiry during pending recovery is a separate liveness policy gap.

The review records three analytic counterexamples, compares a narrower
allocation-free alternative, gives candidate ledger/lifetime rules, and specifies
host reproductions, write/flush/replay cuts, competing logs/accounts, resource
exhaustion and private installed-root/Linux-oracle acceptance. These campaigns
are **NOT RUN**; counterexamples are source deductions, not observed failures.
Earlier stack matrix passes and retained failures above remain separate evidence.

Validation: `git diff --check` PASS; audited source hashes unchanged during review.
Inventory/status evidence:
`.codex-remote-attachments/pkg-recovery-space-review-20261021/manifest.json`.
No new boot acceptance or shared-media preservation measurement is claimed by
this documentation-only audit. Installation/production trust remain disabled.
No build, release-media change, physical write, commit or push was performed.

## Account recovery compatibility candidate and protocol experiment

2026-10-21. Added [PKG2_ACCOUNT_COMPATIBILITY.md](../plans/PKG2_ACCOUNT_COMPATIBILITY.md)
and linked both earlier transaction/recovery-space reviews. Traced the actual
account redo engine, service admission/repeated UPDATEs, scoped EXT4 callbacks,
headroom check, namespace publication and orphan retention. No production source,
account ABI, authentication, boot or allocator behavior changed. Earlier
uncommitted stack fixtures/docs and unrelated AGENTS.md/README.md are preserved.

Candidate: accounts take priority over new package admission/work, with independent
account headroom outside durable package G. Ordinary allocations preserve H+G;
package spending preserves H+G_after; fixed-role accounts can consume H but must
preserve G. Effective account headroom is F-G, not raw F. This is an explicit
proposed compatibility refinement requiring approval and actual EXT4 bounds;
it is not silently implemented. Package-free roots retain existing behavior.
No unlimited live-update/pin or unconditional low-memory progress promise.

Executed `wsl -d Ubuntu-24.04 -- python3 scripts/test_pkg_account_budget_host.py`:
ASan/UBSan PASS. Evidence `pkg-account-budget-20261021-115220` under
`.codex-remote-attachments/` retains copied sources, probe, log and source manifest.
Actual account_redo.c with synthetic maximum-byte inputs and modelled file/data
accounting covers 1/2/4 KiB block sizes. A complete update makes seven writes,
peaks at eight named objects / 57,504 payload bytes, or 57/29/15 data blocks.
Each pending recovery attempt recreates three publication files even when
canonical bytes match; final no-pending replay writes none. A separate live pin
model across 20 updates retains 60 old objects, peaks at 67 total objects and
529/265/133 data blocks. It does not emulate EXT4 metadata, real descriptors,
orphan limits, valid userdb parsing or package grants; no mounted/boot acceptance
or universal account reserve size follows from these figures.

Retained failure `pkg-account-budget-20261021-115200`: incorrect test expectation
of 65 peak objects omitted retired generations during publication; corrected to
67. Log/copied fixture preserved; manifest INCOMPLETE, not PASS. No production
limit or protocol was changed to pass. The successful run's source preservation
check and `git diff --check` PASS; no shared-media hash or boot claim is added.

Next gate is an isolated actual EXT4 account resource envelope (fragmentation,
directory growth, independent block/inode/pool exhaustion, retained generations,
cuts and repeat recovery), followed by reviewed grant/headroom and boot contracts.
Those new mounted/BIOS/UEFI/Linux-oracle campaigns are NOT RUN here. Installation
and production trust remain disabled. Only a private host probe was built;
no release-media build, physical write, commit or push.
