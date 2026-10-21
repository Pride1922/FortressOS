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

## Committed checkpoint and first actual EXT4 account budget matrix

2026-10-21. User authorized commit and continuation. Committed the existing PKG
fixtures/design checkpoint as `73c96f5` (`test(pkg): isolate argument limits and
review recovery space`), leaving unrelated work untouched; no push. Continued
with the isolated actual EXT4 host probe and runner described in
[account compatibility follow-up](../plans/PKG2_ACCOUNT_COMPATIBILITY.md#first-actual-ext4-resource-observations).

Executed `wsl -d Ubuntu-24.04 -- python3 scripts/test_pkg_account_ext4_host.py`:
**20/20 PASS**, ASan/UBSan, Linux e2fsck, exact canonical/sentinel bytes and
asserted ownership/modes. Evidence:
`.codex-remote-attachments/pkg-account-ext4-20261021-120122/`.
Block sizes 1/2/4 KiB x sectors 512/4096, each with small coherent data,
synthetic three-file 8 KiB cap and three updates retaining nine descriptors.
Missing account directory setup and 80 unrelated `/etc` entries are present.
Two further 4 KiB-block tests exhaust real ordinary allocations at the account
floor; account publication succeeds 11 blocks below it. This directly verifies
that the existing fixed-role bypass would defeat an ordinary-only package floor.

Committed free-count deltas (1/2/4 KiB blocks): small 8/8/8 blocks and 8 inodes;
byte-cap 50/26/14 blocks and 8 inodes; retained descriptors 100/52/28 blocks and
16 inodes. Nine old account orphans retain exact old bytes, then close/sync
reclaims all nine. Counts sample callback boundaries; transient gross planner
allocation and arbitrary fragmentation are not bounded by these figures.
Byte-cap profiles use synthetic validation, not maximum valid userdb/service
requests. The floor tests reset their baseline after setup/fill (11 blocks,
5 inodes), unlike the other profiles' pre-setup baseline.

First actual-EXT4 campaign `pkg-account-ext4-20261021-115927/` is retained as an
18/18 pass; expanded run adds floor cases, descriptor byte checks and Linux
ownership/mode assertions. No new failed actual-EXT4 run. Earlier protocol-model
failure remains retained, not reclassified. Source inventory preservation and
`git diff --check` PASS. No shared-media hash, boot, package-grant, physical or
power-loss acceptance claim. Images are disposable regular host files, not disks.

Remaining before choosing A/H: planner gross demand, fragmentation, exact parent
slack limits, inode-only and bounded-pool exhaustion, write/flush/replay cuts and
concurrency with grant accounting. Production trust and installation remain
disabled; no production code, boot/lock/account contract or raw-write gate changed.
Only private host test binaries/images were built; no release-media build or
physical write. Commits are authorized for these PKG checkpoints; no push.

## Extended EXT4 limits, publication and recovery interruption gates

2026-10-21. Continued from `56efd47`. Exact fixtures, failure models and limits
are in [the account compatibility review](../plans/PKG2_ACCOUNT_COMPATIBILITY.md#extended-limits-and-interrupted-account-recovery).
Executed:

```
wsl -d Ubuntu-24.04 -- python3 scripts/test_pkg_account_ext4_host.py
wsl -d Ubuntu-24.04 -- python3 scripts/test_pkg_account_recovery_cuts.py .codex-remote-attachments/pkg-account-ext4-20261021-121959
git diff --check
```

**463/463 primary cases PASS**, ASan/UBSan: 33 profiles plus 430 publication
flush cuts (215 x device sectors 512/4096). Evidence:
`.codex-remote-attachments/pkg-account-ext4-20261021-121959/`.
**344/344 recovery-interruption cases PASS**, ASan/UBSan: 172 account-recovery
flush cuts x both sector sizes, from a Linux-verified pending intent. Evidence:
`.codex-remote-attachments/pkg-account-recovery-20261021-122349/`.
Every interrupted case receives two fresh clean remount/recovery passes, each
audited with Linux e2fsck and exact account bytes (1,548 passes total). Publication
cuts permit complete old/new sets, reject mixtures; recovery cuts require new.
Successful transient images are hashed then removed after both audits; exact
byte dumps/logs, seeds and ordinary-profile images remain. Failed paths retain
their current images. No new failed/partial campaign; older failures stay retained.

Added real fragmented depth-1 extent overwrite planning at 1 KiB, forced `/etc`
block growth across all six geometries, inode-only exhaustion, selected heap
failure, journal-capacity denial, a full 64-orphan table and 1,024 pinned cache
nodes. Resource denials assert zero device writes and retry after restoration.
Cache legacy pins, heap failure and journal-capacity shortening are explicit
private host conditions; no production limit or gate was changed. New images
use 2,048 inodes for capacity cases. Ordinary profile audits also assert sentinel
bytes and canonical ownership/modes; cut audits do not claim full unrelated
metadata/allocation-set comparison.

The host failure model refuses the selected flush and subsequent writes; accepted
writes may persist when closing the stdio image. Fresh mounts execute actual
JBD2 replay/account redo. This is not a physical loss, sector tear, exhaustive
persistence/reordering model, failure during initial journal replay, kernel boot
or package-grant test. Recovery cuts interrupt account redo after root preparation.
Synthetic 8 KiB role data does not prove maximum valid account-service requests.

Primary inventoried-source preservation PASS (empty changes); the separately
derived recovery fixture records its source hash. The new recovery runner,
documentation and opt-in Makefile target are intentional additions outside the
completed original inventory. No shared-media hash or whole-repository PASS is
claimed. No production code/ABI, lock, boot, allocator or bypass changed.
Installation and production trust remain disabled; no release-media build,
physical write or push. Numeric account/package headroom remains unfrozen pending
gross layout bounds, ownership diffs, exhaustive replay/allocation faults and
future combined grant/race acceptance. These are test-only checkpoint changes;
committing does not approve those protected runtime changes.

## Conditional one-pass gross allocation bounds

2026-10-21, continuation from `b283f7e`. Added
[PKG2_ALLOCATION_BOUNDS.md](../plans/PKG2_ALLOCATION_BOUNDS.md) and
`scripts/test_pkg_account_bounds.py`. No production source or admission change.
Source-derived account update bounds are 220/80/35 filesystem blocks at 1/2/4
KiB (220/160/140 KiB), plus seven new inodes. Pending recovery's analytic
one-pass bound is 117/42/18 blocks, plus three new inodes. These exclude setup
directory creation and require existing dense, bounded parent maps with no
allocations beyond EOF; every capacity/ownership/pool check remains separate.

Derivation counts six full-size new files, one intent and all eleven possible
directory insertions. Each insertion pays for a new data block and worst-case
new extent tree, without credit for old frees. Dense <=1 MiB parent maps have
maximum rebuilt trees of 14/4/1 blocks. The existing account floor exceeds these
one-pass raw-space demands. This does not establish restart-invariant headroom
or authorize ordinary/package allocation to consume the account budget.

Executed:

```
wsl -d Ubuntu-24.04 -- python3 scripts/test_pkg_account_bounds.py .codex-remote-attachments/pkg-account-ext4-20261021-121959
```

**26/26 PASS**, ASan/UBSan and Linux e2fsck, with an observer added only to the
copied filesystem allocation accounting. Every actual redo update stays below
the source-derived gross block bound and seven new inodes, including each of
three updates with retained descriptors. Arithmetic enumerates all 1,792 dense
directory extent counts. Existing small/byte/pin/directory and fragmentation
fixtures are observed; maximum-sized adversarial parent maps are not all
instantiated. Synthetic byte inputs retain their prior admission qualification.

Evidence: `.codex-remote-attachments/pkg-account-bounds-20261021-124437/`, with
derived source hashes, per-profile allocation logs and retained images. Its
input is the prior retained workspace; a separate end-of-review hash comparison
checks the original inventoried sources against the current checkout. This is
not an inventory of newly added docs/runner or a shared-media preservation gate.
Opt-in `test-pkg-account-bounds-host` accepts PKG_ACCOUNT_CAMPAIGN explicitly;
it builds from that retained campaign, not implicitly from a different checkout.

No new failed/partial campaign; preceding failure evidence remains intact.
`git diff --check` and target dry-run pass. Next: pure remaining-account-budget
and layout model across interrupted/replayed states, or proof of stable namespace
slots, followed by actual-code differential admission tests. Keep 256 KiB/16
inodes unchanged; no guessed larger reserve or production A/H freeze is justified
yet. Installation, production trust, package grants, boot integration and physical
acceptance remain disabled/unimplemented; no release-media build, physical write
or push. User-authorized commits cover the test/design checkpoint only.

## Finite remaining-budget and namespace replay model

2026-10-21, continued from `7797f0e`. Added
[PKG2_REPLAY_BUDGET_MODEL.md](../plans/PKG2_REPLAY_BUDGET_MODEL.md), a pure Python
state explorer, copied-source evidence runner and opt-in
`make test-pkg-account-replay-model`. No production behavior changed.

Executed `wsl -d Ubuntu-24.04 -- python3 scripts/test_pkg_account_replay_model.py`.
Final campaign: `.codex-remote-attachments/pkg-account-model-20261021-130646/`.
**PASS: 324 profiles, 1,165,689 state instances, 2,324,700 edges**, two negative
arithmetic controls, explicit source-preservation PASS and unchanged inventories.
The finite family spans 1/2/4 KiB blocks, three slot layouts, 1/4/5 initial
directory blocks, zero/one growth block and six canonical-position permutations.
Publication order is unchanged. 216 profiles admit; 108 correctly reject.

Model rule: `remaining = worst future allocation peak - already allocated`.
Allocated stages, persistent directory growth and retired objects stay charged;
rename moves one owned object, and verified unpinned orphan reclaim credits its
resources exactly once. A fixed independent package grant remains protected in
both block and inode dimensions at every transition and transient peak. No grant
is actually provisioned or enforced in the kernel by this experiment.

Found and retained an important namespace counterexample: complete two roles,
leave a later empty publication temporary, restart and leave another later
temporary, then restart at the first role. Suitable slots can all be occupied
when no growth remains; enough raw free space cannot guarantee recovery. Saved
program-counter traces identify the blocked create and durable role state.
Mixed layouts can finish uninterrupted yet reject restart-safe admission.
This is an abstract counterexample requiring actual mounted-backend reproduction;
it does not change cleanup order or demonstrate a new observed kernel failure.

Earlier raw PASS `pkg-account-model-20261021-125919` is explicitly disqualified
as valid-layout evidence because its seed could end with an invalid four-byte
directory record. It had already completed before an attempted interrupt;
original output remains unchanged, with separate qualification.json. Corrected
`130047` passes valid-seed checks; `130304` adds counterexample traces; final
`130646` additionally asserts the uninterrupted-versus-restart distinction.
No rejected profile is reclassified as an admitted one to obtain a green gate.

This is a finite model, not actual C execution, ASan/UBSan, Linux-oracle,
firmware, physical or universal-proof acceptance. It omits live pins, arbitrary
writers, real allocation groups and bounded pools, journal sectors, malformed
records and actual account/package concurrency. Peak remaining headroom in the
admitted finite family is 53/23/11 blocks plus three inodes; those figures do not
replace the source-derived U/R bounds or freeze production headroom.

Next gate: reproduce the saved two-interruption slot witness in actual EXT4 and
refine a restart-safe namespace/remaining-budget admission profile. Admission
must derive state from validated retained ownership, never reset a constant
grant or add allocated stage bytes to free credit. Protected runtime changes
remain subject to review. `git diff --check` and target dry-run pass; no release
build, physical write, production trust, installation or push.

## Actual namespace replay witness (2026-10-21)

[Review and exact evidence](../plans/PKG2_NAMESPACE_REPLAY_REVIEW.md): private
campaign `pkg-account-namespace-20261021-132851` passes 30/30 actual EXT4/JBD2/
redo terminal states under ASan/UBSan and Linux fsck/debugfs. Six combinations
(1/2/4 KiB blocks x 512/4096-byte sectors) each cover an uninterrupted success,
two durable empty-temporary interruptions and two fresh blocked remounts.
Each blocked remount's same-mount retry performs zero writes (12/12 retries).

The model's saved witness is now observed at the unchanged real 1 MiB directory
limit. After two interruptions, empty group/shadow publication files leave zero
suitable slots for passwd publication. Recovery returns EFBIG while preserving
the pending record, all stages and mixed canonical bytes. Free block/inode
counts are 27045/996, 13514/996 and 6750/1764 at 1/2/4 KiB respectively, in both
sector modes. More raw headroom cannot fix this namespace constraint.

Read-only observation passes 30/30 directory-checksum/slot/bitmap snapshots.
UNINIT bitmap reserved allocations are counted from group descriptors and their
physical bit positions are explicitly not enumerated. This is not complete
exclusive-ownership attribution or a proof of the model's allocation peaks.
Primary source inventory comparison passes; observer input-image hashes remain
unchanged. Whole repository/media preservation and boot acceptance are separate.

Retained attempts: `132404` invalid eight-byte free-record seed rejected by
Linux; `132531` stopped during slow stdio-read control; `132703` successful
uninterrupted control followed by sentinel mode expectation failure. Separate
qualification files preserve their original logs/status. The first allocation
observer also failed on UNINIT bitmap handling; original source/reason and the
corrected passing observer remain in the accepted campaign.

Next: specify and prove restart-safe namespace admission or review a persistent
capacity mechanism, then refine remaining resource costs against actual
planners/pools and replay. Production cleanup order, reservation floors, mount
admission and boot routing are unchanged. No installation, production trust,
release media, physical write or push; prior private Ring 3 matrix acceptance
and its preservation checks are not rerun or broadened by this host gate.

## Conservative restart-safe `/etc` admission candidate (2026-10-21)

[Profile, proof and limitations](../plans/PKG2_NAMESPACE_ADMISSION.md): six exact
24-byte records, free or owned by the six fixed account names, plus constrained
smaller records/immutable slack. The count includes occupied account records;
it is not six free records reacquired on reboot. Create/delete/rename preserve
the physical records, and either insertion has at most five occupied records.
No directory growth or cleanup-order change is needed under this narrow profile.

Finite pure-model campaign `pkg-namespace-admission-20261021-134420`: 216 profiles,
1,033,089 summed state instances PASS. All 54 six-record profiles preserve
admission at every explored state and have no blocked forward step. Seven
negative controls pass. The remaining 162 smaller profiles reject conservatively;
some would complete, but 36/18/18 three/four/five-record profiles contain blocked
states. Six is sufficient, not a universal necessity or a guessed resource floor.

Actual-code campaign `pkg-namespace-admission-ext4-20261021-134539`: 24/24 states
PASS under ASan/UBSan and Linux fsck/debugfs, across three block sizes and two
sector sizes. Each pair executes the same two durable interruptions as the
retained failure witness, then completes recovery and passes a fresh repeat.
All 48 before/after structural observations accept; all three initial seeds
pass Linux, and original rejected three-record seeds remain unchanged.
Readback checks canonical/staging bytes, metadata and unrelated sentinel;
completed states have neither pending intent nor publication temporaries.

Gate-source preservation comparisons pass; prior source inventory is verified
in the copied workspace before compilation. These are host/source observations,
not whole-checkout/media preservation or fresh BIOS/UEFI/SMP acceptance.
No failed/partial run occurred in this continuation; previous retained failures
remain qualified. Opt-in targets `test-pkg-namespace-admission` and
`test-pkg-namespace-admission-ext4 PKG_NAMESPACE_CAMPAIGN=...` dry-run correctly.

This is a conservative test specification, not a production admission scanner.
Normal compact directories can reject. Authenticated ownership/alias validation,
private staging-parent capacity, persistent exclusion of other namespace writers,
grant/resource integration and mount failure routing remain open. The foreign-
writer negative control explicitly invalidates admission when it steals a bin.
Next gate addresses those private-parent and fence prerequisites. Installation,
production trust, provisioning, release media, physical writes and pushes remain
disabled; concurrent USB work is preserved separately.

## Opt-in namespace scanner and mount-lifetime fence (2026-10-21)

[Implementation, lifecycle and exact evidence](../plans/PKG2_NAMESPACE_KERNEL.md).
Added `ext4_root_account_namespace_admit()` without any production caller. It
validates the actual six-record `/etc` and five-record private account parent,
role metadata/link counts/aliases and root/ancestor identities under existing
filesystem exclusion, then atomically arms both fences. Admission does not
write, create or relayout directories. No package grant or disk marker exists.

The fence blocks ordinary creators, unlinkers, renamers, replacements and child
data/metadata writers, plus ancestor relocation and metadata changes. Fixed-role
account callbacks use a separate scoped namespace-operation flag; the allocator
bypass/floor policy is unchanged. The fence has no process-owner release or
unfence operation. It survives account writer exit for this mount, but a future
production caller must explicitly re-admit on every remount before publication.
Existing roots without opt-in retain their behavior; boot/installer code is unchanged.

Primary `pkg-namespace-kernel-20261021-140820`: **72/72 states PASS**, actual
EXT4/JBD2/redo ASan/UBSan + Linux fsck/debugfs, three block sizes x two sector
sizes. Includes 24 interrupted/completed redo states, 42 zero-write API refusals
and six legacy no-opt-in controls. Every admitted process checks root-actor
fence denials, unrelated `/var` progress, idempotence, taint-latched fencing and
published-root refusal. Host scanner frame maximum 640 B; source inventory
comparison passes with no changed inventoried sources.

Supplement `pkg-namespace-regression-20261021-142535`: **6/6 profiles PASS**:
actual legacy account service x2, corrupt-intent/retained-foreign-handle/pthread
writer overlap x2 and ownership denials x2. Actual redo unit suite 2586 cases /
49 operation boundaries PASS; freestanding EXT4 object compile PASS, new scanner
frame maximum 272 B. Neither object compilation nor host pthread tests establishes
new boot/SMP/IRQ acceptance. The corrupt record remains semantically rejected
despite structural admission, with detached root and zero redo writes.

Private-parent geometry model `pkg-private-namespace-20261021-141816`: **6141
shapes / 58017 edges PASS**, including rename with source still present and the
four-slot negative. Its C references are not executed by the Python model.

Retained attempts: `140235` interrupted after allocator-bypass scope review;
`140456` FAIL in bad-mode harness setup before the scanner. Final code separates
namespace authorization from allocation policy and negative cases skip setup.
Original logs/status and qualifications remain. Source inventories and private
image preservation are distinct from whole-repository/media or boot acceptance.

Next: durable package marker/grant/resource admission and reviewed mount failure
routing, then production re-admission integration. Structural capacity alone is
not database authentication, guaranteed I/O progress or permission to install.
Installation, production trust/provisioning and grants remain disabled. No release
media, physical writes or pushes; concurrent USB changes are left separate.

## 2026-10-21 — durable grant and mount-admission candidate

[Candidate design](../plans/PKG2_DURABLE_GRANT_DESIGN.md) now specifies explicit
enrollment/boot authority, fixed marker/ledger/ownership records, resource
conservation, transaction phases and failure routing. Durable grants survive
owner death; actual tagged reclamation refunds once, baseline frees do not refund,
and publication transfers ownership without inventing free resources. Missing or
corrupt required metadata never falls back to a zero grant or writable legacy root.

Review found two integration blockers: account allocator bypass must explicitly
preserve package escrow, and reconstruction must precede current constructor
orphan cleanup as well as `accounts_prepare`. A new internal preview validator is
needed; the existing detached-root public namespace API cannot run that early.
The `/var` 0700 versus namespace-profile 0755 mismatch remains an explicit
enrollment refusal, with no automatic permission change. Boot selector authority,
production bounds and recovery capability expiry/revocation remain review gates.

Private `pkg-grant-design-20261021-164510`: finite Python model **PASS**, 4900
states / 21456 edges / 42912 complete before-or-after restart selections; nine
independent block/inode boundary cases; ten mount-policy cases; 40320 permutations
with exactly one satisfying the prescribed serial order; eleven negative controls
detected. Restart selections assume atomic journal outcomes: they do not test real
writes/flushes. Order enumeration checks the specified policy, not actual boot
control flow. No production quota, binary parser, C/JBD2 integration, Ring 3,
SMP or boot acceptance follows. All retained run output and the source snapshot
are under `.codex-remote-attachments/pkg-grant-design-20261021-164510/`.

The listed source/AGENTS/README hashes match across model execution; this is
separate from boot acceptance and is not a whole-repository/media preservation
claim. Concurrent USB/HID work remains untouched. No kernel or release-media
build, installation, production trust provisioning, push or physical write was
performed. No failed model run occurred in this checkpoint; earlier retained
namespace failures remain recorded above.

Next: review the candidate contract and implement a private actual-code ledger
codec/planner adapter with malformed-record and atomic accounting cut tests,
before considering protected mount hooks. Installation remains disabled.

## 2026-10-21 — private candidate ledger codec and accounting adapter

Implemented `tests/pkg_grant_candidate.{h,c}` and its standalone host gate, with
no production callers/build linkage or mount changes. Exact-length decoding,
hash/phase/reserved-field checks and canonical wire copying preserve outputs on
failure. Accounting checks block and inode dimensions together, sequence overflow,
actual-free conservation and escrow/floor preservation. Provenance and authority
remain caller preconditions; marker/table parsing and actual filesystem journal
integration remain unimplemented.

Invocation: `wsl -d Ubuntu-24.04 -- make -f tests/pkg_grant_candidate.mk
test-pkg-grant-candidate` (one command). Final evidence
`.codex-remote-attachments/pkg-grant-codec-20261021-170458-298231/`:
ASan/UBSan **PASS**, 1638 independent Python-generated wire fixtures, 1025
byte-tear positions, 132132 accepted and 1117068 rejected arithmetic plans,
528528 mock complete before/after restart selections. Accepted/rejected plans
cover both ordinary-allocation headroom and reserve-boundary refusal. Block-side
success followed by inode refusal preserves the entire output. Freestanding
adapter-only compilation **PASS**, maximum measured frame 336 B (not cumulative
call-stack depth). Exact command/output/fixture/source-hash manifests retained.

Retained attempts: `170305-032066` compile FAIL because the project string header
does not declare `strrchr`; harness changed to an explicit path scan.
`170316-950150` PASS preceded additional positive ordinary-allocation headroom,
charged-ledger and unknown-profile coverage. No attempts were removed.
`170346-278211` also PASS; final `170458-298231` repeats it after enlarging the
two restart fixture read buffers to accommodate the loader's 1025-byte bound,
including a possible oversized input. Generated restart fixtures remain 1024 B.

Listed input/source/AGENTS/README hashes match across each run. Concurrent work
was neither staged nor reverted; concurrent repository commits are independent
of this scoped hash check. No whole-repository or media preservation claim is
made. No boot, Ring 3 or real JBD2/flush-cut acceptance was run. The codec checks
ledger structure only; budget snapshots assume atomic commit and do not prove it.
Installation, production trust/provisioning, release-media builds, pushes and
physical writes remain disabled.

Next: private marker/ownership-table validation and identity/role cross-binding,
then a reviewed actual-JBD2 adapter for atomic ledger/table/bitmap cut tests.
Production mount integration remains later work.

## 2026-10-21 — private marker/ownership record cross-validation

Added `pg_validate_set` to the private candidate adapter. It checks marker
structure and separate expected identity/hash/locator bindings; ledger/table
transaction, epoch and sequence; complete table hash; unique typed roles/inodes;
fixed-metadata aliases; class/flag/tombstone rules; padding and unused slots; and
overflow-safe charged totals. Failure leaves output unchanged. Independent
expectations are an API precondition, not a newly implemented boot trust source.
The private role profile and its narrower accepted layouts are documented in
[the candidate design](../plans/PKG2_DURABLE_GRANT_DESIGN.md).

Invocation: `wsl -d Ubuntu-24.04 -- make -f tests/pkg_grant_candidate.mk
test-pkg-grant-candidate` (one command). Final evidence
`.codex-remote-attachments/pkg-grant-codec-20261021-172044-337449/`:
ASan/UBSan **PASS**, 4101 record sets (8 valid, 4093 refused), including every
reserved table-header/tail byte and every unused slot, rehashed semantic defects,
cross-record mismatches, external UUID/profile/epoch/hash/inode/generation
mismatches, aliasing, invalid classes/flags/roles, charged-sum overflow and
independent exact-length failures. Existing 1638 ledger fixtures, 1025 byte-tear
positions, 132132 accepted / 1117068 rejected arithmetic plans and 528528 mock
restart selections also **PASS**. Freestanding adapter-only compile **PASS**:
new set validator frame 208 B, hash helper 208 B; whole adapter maximum frame
336 B. These are individual frames, not cumulative stack measurements.

Retained attempts: `171942-331145` FAIL on the valid orphan-flag fixture: Python
wrote an eight-byte value into a four-byte flags field and damaged its UUID.
Corrected the fixture width; `171959-795697` PASS (110 sets) before expanded
padding/null-input/context-generation coverage. All attempts/logs remain.

Listed source/AGENTS/README hashes match across the final run; snapshots and exact
commands/stdout/stderr are retained. This scoped source check is separate from
boot acceptance and does not establish whole-repository or media preservation.
No new boot/Ring 3/JBD2/physical acceptance, release-media build or production
mount/allocator integration occurred. Actual inode generations, extent overlaps,
allocation bitmaps and linked/orphan provenance are not established by record
bytes. Whole-filesystem rollback and independently trusted selector construction
remain outside this adapter. All installation, production trust provisioning,
push and physical-write restrictions remain in force.

Next: private actual-filesystem identity/map/ownership refinement, followed by
real JBD2 ledger/table/bitmap atomic-accounting cut tests. Keep production mount
admission and installation disabled through those gates.

## 2026-10-21 — actual filesystem refinement and real JBD2 interruption

Added a [private EXT4/JBD2 adapter](../plans/PKG2_FILESYSTEM_GRANT_ADAPTER.md):
checksum-checked actual inode generations, modes/owners/links, initialized dense
maps, allocation counts and whole-filesystem ownership/bitmap auditing refine
the marker/ledger/table claims. One linked charged payload is supported; other
layouts refuse. Real block allocation/truncate planners stage grant records as
journal metadata in the same transaction as inode/bitmap/counter changes.
Production sources, mount paths and allocator policy are unchanged.

Standalone WSL invocations use `make -f tests/pkg_grant_candidate.mk` followed by
`test-pkg-grant-fs`, `test-pkg-grant-fs-negative` or
`test-pkg-grant-fs-recovery`. Exact commands/binaries/logs/images/manifests remain
under `.codex-remote-attachments/`:

| Campaign | Actual host result | Separate Linux/source evidence |
| --- | --- | --- |
| `pkg-grant-fs-20261021-172846` | 6/6 geometries; 6864 operation write/flush cuts, two mount cycles per cut; charge/refund coupled accounting PASS under ASan/UBSan | 18 clean e2fsck snapshots; 18 independent debugfs byte/metadata oracles PASS; original broad source guard FAIL on concurrent USB/HID changes, compiled-dependency reconciliation unchanged |
| `pkg-grant-fs-negative-20261021-174323` | Final 6/6 geometries, 108 zero-write refusals plus 2 single-group eligibility refusals PASS under ASan/UBSan | 6 clean restored-image Linux audits; captured compiled inputs unchanged |
| `pkg-grant-fs-recovery-20261021-174333` | Final 6/6 eligible geometries; 2816 actual replay write/flush cuts, two exact committed-state checks per cut PASS under ASan/UBSan | 12 clean e2fsck snapshots; 12 independent debugfs byte/metadata oracles PASS; captured inputs unchanged |

Geometry is 1/2/4 KiB filesystem blocks × 512/4096-byte sectors, not firmware or
SMP. Four persistence modes × cut-before/after each callback apply to both
operation and replay campaigns. No sector tearing or physical persistence is
claimed. Operation tests admit valid coupled old/new states; replay tests demand
the exact committed sequence and grant totals. Selected Linux snapshots verify
payload bytes, hashes, generations, ownership, modes, sizes and block counts;
Linux does not inspect every transient cut image. Primary operation cuts precede
the later private eligibility/headroom additions; final refusal and replay suites
exercise those additions without changing the transaction planner/commit path.

Negative checks include stale claimed/actual generations, false size/allocation
counts, wrong owner/mode/link count, cross-inode extent alias, a bitmap-freed
referenced block with repaired checksums/counters, unsupported orphan/victim
claims, heap/read errors, damaged backup identities and UINT64_MAX escrow.
Output preservation and zero further write/flush callbacks are asserted after
fixture preparation/injection. Corrupt actual inode/bitmap cases modify only
private host-memory images, restored before the final Linux audit.

**Retained real failure and restriction:** `pkg-grant-fs-recovery-20261021-173724`
passed four profiles then failed on a 16 MiB single-group 4 KiB/512-byte fixture.
`174103` reproduced charge replay failure at persistence=all, cut-before 27,
constructor `journal-source`, EIO; failing image retained. Partial superblock
checkpoint cannot bootstrap without the existing group-1 backup identity. The
private grant profile now refuses single-group roots and validates group-1 SB/GDT
backup identities before preparation; final 4 KiB replay fixtures have two groups.
This exclusion is tested, not a production recovery fix. The original failure
remains open for that excluded geometry. Future package admission must enforce
the restriction or separately review support. Production mount behavior remains
unchanged.

Other retained attempts: `pkg-grant-fs-20261021-172832` compile FAIL (host
permissions test macro missing); `pkg-grant-fs-negative-20261021-173305` fixture
FAIL (baseline-class mutation omitted inode-total adjustment); `173359` passed
90 runtime refusals/Linux but broad preservation guard failed on unrelated USB
edits; `pkg-grant-fs-recovery-20261021-173704` fixture FAIL (4 MiB journal too large
for an 8 MiB filesystem). No failing attempt was removed. Review also fixed
overflow in the new private headroom comparison, covered by the final refusals.

Broad source guard failures remain distinct from actual-code test outcomes.
Separate compiler-dependency reconciliation shows the concurrent USB changes
were outside these probes; prior immutable source snapshots are retained. This
is neither a whole-repository preservation claim nor release-media/boot evidence.
Concurrent USB/mouse work, AGENTS.md and README.md were not staged or reverted.

Limits/next: inode allocation/free is fixture setup (inode stays charged); atomic
inode grant spend/refund, external-tree positive cases, orphan/pinned reclamation,
concurrent ordinary/account writers, namespace capacity and authenticated pending
recovery still need acceptance before protected mount integration. No new Ring 3,
BIOS/UEFI, SMP, installation, production trust/provisioning, release-media build,
push or physical-write acceptance is claimed or enabled.

## 2026-10-21 — inode grants, pins and orphan reclamation (private overlay)

[Lifecycle adapter and limits](../plans/PKG2_GRANT_LIFECYCLE_ADAPTER.md).
The actual namespace planner allocates the target inode and atomically charges
it with grant/table metadata. Actual writes charge a data block; unlink retains
both charges while pinned. A duplicated VFS file reference delays final close.
Original bytes remain readable after unlink. Actual orphan steps refund only
reclaimed blocks, then the released inode, with RETIRED provenance and generation
retained. Repeated recovery after completion produces no write/flush callback.

Integration is deliberately test-only: a copied workspace renames the original
semantic commit so a private wrapper can add record images, and calls an explicit
engine-based reconstruction validator before constructor orphan cleanup. The
two exact overlays and original/modified hashes are in each manifest. Shared
production filesystem sources and boot behavior are unchanged. Grant-aware
read-only preview before replay, trusted enrollment and production policy are
still unimplemented; the private reconstruction hook runs after replay/activation.

Invocation: WSL `make -f tests/pkg_grant_candidate.mk test-pkg-grant-lifecycle`;
retained campaigns used `python3 scripts/test_pkg_grant_lifecycle.py --block N`
to cover the same matrix in separate copied workspaces.

| Block / sector bytes | Callback cuts | Retained campaign |
| --- | ---: | --- |
| 1024 / 512 | 3568 | `pkg-grant-lifecycle-20261021-175934` |
| 1024 / 4096 | 1928 | same |
| 2048 / 512 | 5312 | `pkg-grant-lifecycle-20261021-180313` |
| 2048 / 4096 | 1544 | same |
| 4096 / 512 | 8800 | `pkg-grant-lifecycle-20261021-180336-111592` |
| 4096 / 4096 | 1352 | same |

Actual C ASan/UBSan runtime checks **6/6 PASS**, **22504 cuts / 45008 complete
mount/recovery cycles**. By operation: create 3544, pinned unlink 3816, final close
6952, constructor orphan recovery 8192 cuts. Every write/flush callback is cut
before/after under cached/all/alternating/inverse persistence. Each case injects
one failure followed by two complete mounts; this is not an exhaustive sequence
of repeated recovery failures or arbitrary sector tearing. The stdout `commits`
metric counts acknowledged returns, not all possibly durable uncertain commits.

Linux **12/12 e2fsck audits PASS**. A separate read-only debugfs oracle on those
12 retained snapshots verifies free target-inode bit, retained generation, zero
links/blocks, absent name, empty orphan head, record hashes and full escrow refund.
These independent byte/metadata checks cover selected snapshots, not every cut.
Exact logs, generated C, copied sources, overlays, binaries and oracles remain
under `.codex-remote-attachments/<campaign>/`.

**Preservation is separate:** all compiled workspace copies stayed byte-identical
during their runs. The 1 KiB repository-input guard passes. The 2/4 KiB original
guards FAIL because concurrent `console.h`/`input.h` edits added unused mouse/input
function declarations. Separate retained rebuilds before/after substituting just
those declarations compare every SHF_ALLOC ELF section except build ID; matches
qualify those header changes, not the whole current checkout. Original failures
remain in manifests. Other concurrent USB, input/console and nano edits are
outside this acceptance scope and were neither staged nor reverted.

Retained attempts: `175736` strict compile FAIL on harness indentation; `175814`
ASan/UBSan FAIL because the early helper incorrectly staged through unpublished
`fs->engine`. Corrected it to use the constructor's explicit engine, without
faking flags or publishing a root. A parallel 4 KiB launch collided with the
second-resolution `180313` directory and refused before copying/compiling; the
2 KiB workspace was not overwritten. Collision evidence is retained there and
run names now use microseconds. No attempt was removed.

Limits: one tagged regular data file and one-block allocation, no unrelated
orphan/ordinary/account writer, extent-tree positive case or multi-batch reclaim.
Refund restores G, not ordinary headroom: fixtures remain RESERVED with T=(8,1).
Clearing tombstones, releasing unused escrow to IDLE and new-writer admission
remain next protocol gates, followed by competing-allocation/resource admission.
Host resets reuse a synthetic external identity context, not fresh trusted boot.
No Ring 3, BIOS/UEFI, SMP/IRQ, physical, installation, production trust/provisioning,
release-media build or push was performed or enabled. AGENTS.md/README.md and all
concurrent work remain preserved.


## 2026-10-21 private unused-escrow release to IDLE

The private pre-intent abort now publishes IDLE in one actual JBD2 metadata
transaction. It clears the ownership table (including retired entries), paired
transaction identities, total/remaining/charged budget, operation and plan; it
increments the paired sequence and preserves the unchanged installed-state
hash/generation and enrollment bindings. Free block/inode counts stay unchanged.
A repeated IDLE release performs no write/flush or sequence increment.

This is a copied-workspace host adapter, not a production package release API.
The shared lifecycle reconstruction validator now accepts canonical zero-budget
IDLE as well as its existing fixed RESERVED profile. Production sources, mount
ordering, trust provisioning, installation and ordinary/account allocator behavior
are unchanged. Linked/pinned objects and sequence exhaustion are tested to refuse
before writes. Release also checks the entire directory for unexpected children;
that additional untracked-child branch is reviewed, not separately fault-injected.
The transition supports only pre-intent RESERVED abort, not post-install RETIRING.

Invocation: WSL `python3 scripts/test_pkg_grant_idle.py --block N` for each of
1024/2048/4096 (equivalent to the standalone `test-pkg-grant-idle` target).
Each profile covers an unused reservation and a real create/write/pinned-unlink/
last-close refund followed by release. Every release write/flush callback is cut
before/after under cached/all/alternating/inverse persistence. Two mounts follow
each cut: the first validates old RESERVED or new IDLE and explicitly finishes an
old outcome; the second verifies completed IDLE. Every mount checks unchanged
actual free counters. This does not interrupt replay again or prove arbitrary
chains of recovery failures. Sector callbacks remain atomic; identity expectations
are synthetic and retained across host resets. No boot, BIOS/UEFI, Ring 3, SMP,
IRQ or physical acceptance is claimed.

Selected completed images are checked with Linux e2fsck and independent debugfs
record dumps: ledger/table self hashes, cross hash, paired sequence, zero budgets,
transaction IDs, cleared ownership entries, equal old/current state identity and
absent payload name. Those Linux audits do not cover every transient cut image.

The preliminary `pkg-grant-idle-20261021-183244-212328` run passed runtime checks
but its repository compiled-input guard FAIL is retained: `tests/pkg_grant_idle.inc`
changed while I added sequence-exhaustion coverage. Its copied inputs stayed
unchanged. It is excluded from final acceptance totals; the final 1 KiB rerun
uses `pkg-grant-idle-20261021-183425-353224`. No failure was deleted.

The pure codec/budget regression also PASS:
`pkg-grant-codec-20261021-183453-055848` reports 1638 vectors, 1025 byte cuts,
132132 accepted / 1117068 rejected arithmetic plans, 528528 mock restart
selections and 4101 record sets (8 accepted / 4093 rejected). These remain pure
value/mock checks, separate from the actual-JBD2 interruption campaign.

Next: competing-allocation/resource admission and a fresh writer after IDLE,
then grant-aware validation before the first replay write and trusted boot
integration. Installation, production trust provisioning, release-media builds,
pushes and physical disk writes remain disabled. Concurrent USB/nano work was
committed separately by its owner as `399cf94` during this turn and is preserved.


Final actual-code ASan/UBSan results **6/6 PASS**, **4912 callback cuts / 9824
mount cycles**, **12/12 Linux e2fsck and selected record audits PASS**:

| Block / sector bytes | Cuts | Campaign |
| --- | ---: | --- |
| 1024 / 512 | 880 | `pkg-grant-idle-20261021-183425-353224` |
| 1024 / 4096 | 480 | same |
| 2048 / 512 | 1168 | `pkg-grant-idle-20261021-183324-222424` |
| 2048 / 4096 | 352 | same |
| 4096 / 512 | 1744 | `pkg-grant-idle-20261021-183338-310485` |
| 4096 / 4096 | 288 | same |

**Preservation is separate from runtime acceptance:** all three final manifests
report unchanged copied compiled inputs and unchanged repository compiled inputs.
Original fixture images also retain their pre-run hashes. These are host image
and source checks, not release-media or boot acceptance. Full logs, exact commands,
generated C, copied sources, overlay hashes, binaries and selected snapshots are
retained in the campaign directories. Aggregate manifest: `pkg-grant-idle-summary-20261021-183956/manifest.json`.


## 2026-10-21 private competing allocations and fresh reservation admission

Implemented `tests/pkg_grant_compete.inc` and the standalone
`test-pkg-grant-compete` copied-source gate. No production filesystem/allocator
source, mount policy, installation path or syscall is changed. A generated
translation unit retains the existing lifecycle and IDLE helpers and wraps only
its semantic commit dispatch. Exact generated source and the two production-copy
overlays remain in each campaign workspace/manifest.

Ordinary real create/write plans must preserve staged F minus durable G above
max(the existing numeric A0, synthetic R_test) for both blocks and inodes. The
private predicate enforces the A0 minimum internally. Tagged plans still use the
actual inode/bitmap lifecycle wrapper to debit/refund G in the same journal
transaction as allocation changes. Ordinary success leaves ledger/table unchanged.
The fixture reaches exact effective block and inode limits, rejects +1 before
writes with unchanged write offset/create output, then still creates/writes a
tagged object using its escrow. Pinned unlink keeps the grant and blocks another
admission. Actual last-close refund followed by IDLE release lets ordinary create
and write spend previously escrowed space. An ordinary write winning before a
fresh admission causes that admission to refuse based on freshly read F.

Fresh admission checks canonical IDLE, full actual ownership, expected sequence,
empty transaction directory, absent pending.v1, nonzero synthetic transaction ID,
and healthy/idle engine state. Exact headroom equality admits; +1 required floor
in either dimension refuses without writes. RESERVED and its new transaction/plan,
paired sequence and (8,1) empty grant are one actual JBD2 metadata transaction.
Stale/duplicate/zero-ID and pending-record requests refuse without writes. The
fixed installed-state and authorization identities remain synthetic fixture
context; there is no signature/catalog revalidation or task-owned session here.

The fault campaign cuts every admission write/flush callback, before/after, with
cached/all/alternating/inverse persistence. Two complete mounts independently
observe coherent IDLE or RESERVED and the matching sequence/transaction, without
completing IDLE between those two observations. Physical free block/inode counts
must remain unchanged for normal admission and after every cut/recovery. After
the campaign an old outcome is explicitly admitted, then a second tagged inode
create/reclaim/release cycle returns to IDLE. Linux e2fsck and debugfs audit
selected RESERVED/IDLE snapshots, record hashes/crossbinding, empty ownership
entries, absent tagged name and exactly three blocks of original competitor C
bytes. Those independent audits do not inspect each transient cut image.

Preservation is a separate gate from runtime acceptance; there is no boot
acceptance claim. Each final campaign retains original fixture hashes, copied
compiled-input hashes and a repository compiled-input guard. Serialized host
calls exercise actual filesystem exclusion, not parallel process/SMP races.
Sector callbacks are atomic, replay is not interrupted again, and ordinary
competitor allocations themselves are not fault-injected in this campaign.
Elevated R_test tests effective headroom without physical near-full fragmentation,
parent expansion or positive external extent-tree allocation. Account BEGIN,
UPDATE/redo/bypass paths and actual numeric R_acc bounds remain unimplemented.
Writer admission here is durable reservation admission; task-retained exclusive
sessions, cancellation/in-flight ownership and capability checks remain open.

Retained preliminary evidence:
- `pkg-grant-compete-20261021-184317-818722`: C ASan/UBSan PASS for 440 cuts;
  Python Linux-byte oracle FAIL expected four blocks although only three writes
  succeeded. The dump has the correct 3072 bytes. Corrected expectation to three;
  pending/zero-ID checks were also added before final fixtures. This attempt is
  excluded from acceptance totals; its manifest remains INCOMPLETE and records
  the changed private test input.
- `pkg-grant-compete-20261021-184414-942773` and
  `pkg-grant-compete-20261021-184438-397789`: 1/2 KiB runtime, Linux and source
  preservation PASS before the later exact-admission-boundary strengthening.
- `pkg-grant-compete-20261021-184448-579162`: 4 KiB runtime and Linux PASS,
  original repository compiled-input guard FAIL because I strengthened
  `tests/pkg_grant_compete.inc` while it ran. Copied inputs stayed unchanged;
  the failure is retained. The strengthened final matrix is reported separately.

Next gates: actual account-planner recovery bound and bypass enforcement,
process-owned writer lifetime, then grant-aware validation before the first
replay write and authenticated boot enrollment. This work enables neither
production integration nor installation/trust provisioning. No release-media
build, push, physical write, BIOS/UEFI or Ring 3 run was performed. All concurrent
work, including AGENTS.md and README.md, is preserved.


Final strengthened actual-code ASan/UBSan matrix **6/6 PASS**, **2456 admission
callback cuts / 4912 complete recovery mount cycles**, **12/12 Linux e2fsck and
selected record/competitor byte audits PASS**:

| Block / sector bytes | Cuts | Campaign |
| --- | ---: | --- |
| 1024 / 512 | 440 | `pkg-grant-compete-20261021-184731-744714` |
| 1024 / 4096 | 240 | same |
| 2048 / 512 | 584 | `pkg-grant-compete-20261021-184755-993280` |
| 2048 / 4096 | 176 | same |
| 4096 / 512 | 872 | `pkg-grant-compete-20261021-184806-143258` |
| 4096 / 4096 | 144 | same |

Invocations: WSL `python3 scripts/test_pkg_grant_compete.py --block N` for each
of 1024/2048/4096, equivalent to the standalone Make target's complete matrix.
The final runs include exact-equality admission success and +1 refusal in each
dimension, the internal max(A0,R_test) predicate, and unchanged physical free
counts through every recovery. **Preservation separately PASS:** all three final
manifests report no changed copied compiled inputs and no changed repository
compiled inputs; their private test include hashes agree. Original fixture image
hashes also match. No new boot or release-media preservation claim is made.
Aggregate evidence: `pkg-grant-compete-summary-20261021-185143/manifest.json`.


## 2026-10-21 private account capacity and process-owner lifetime

Implemented the conditional account-role capacity certificate and a separate
process-owned value arena. Exact derivation, APIs and limits are in
[the capacity/owner plan](../plans/PKG2_ACCOUNT_CAPACITY_OWNER.md). Production
account service, process/descriptor lifecycle and boot code are unchanged.

For a fenced, dense, unpinned account namespace, C is (83,11), (38,11), (20,11)
blocks/inodes for 1/2/4 KiB blocks; R_acc=C minus actual allocated role maps and
inodes. Empty creates consume an inode. Actual staged free-counter deltas update
the private tracker at semantic commit, while callback-end role scans independently
recompute it. Exact F-G=R_acc admits; +1 escrow in either dimension refuses before
writes. Retained account readers refuse without publishing an output. This is a
source-derived profile bound with actual planner refinement, not a general bound
for account pins, arbitrary namespace growth, semantic authorization or concurrent
writers. The capacity tests use maximum-byte synthetic records and volatile G;
they do not call ACCOUNT_BEGIN/UPDATE or an enrolled persistent package ledger.

Final actual EXT4/account redo ASan/UBSan matrix: **18/18 completed cases PASS**
across 1/2/4 KiB blocks � 512/4096-byte sectors � recover/update/interrupt modes.
The interrupted durable empty group-publication create is followed by two fresh
host-process recoveries: **30 runtime invocations**, **762 acknowledged
semantic commits**, **1836 successful capacity snapshots**. Commit counters
count successful returns, not every possibly durable uncertain outcome. Linux
**18/18 e2fsck and final byte audits PASS**; independent read-only debugfs capacity
oracles **18/18 PASS**, checking actual i_blocks, final roles, orphan head and
unchanged F-R_acc between input and completed output. The latter checks the test
escrow potential across recovery without claiming a durable ledger integration.

| Block bytes | Pending-recovery initial R (blocks,inodes) | Final canonical R | Campaign |
| --- | --- | --- | --- |
| 1024 | (41,3) | (59,8) | `pkg-account-capacity-20261021-191835-971677` |
| 2048 | (16,3) | (26,8) | `pkg-account-capacity-20261021-191845-933321` |
| 4096 | (8,3) | (14,8) | `pkg-account-capacity-20261021-191856-158449` |

Each campaign covers both sector sizes and all three modes. Invocations:
WSL `python3 scripts/test_pkg_account_capacity.py --block N`; independent oracle:
`python3 scripts/pkg_account_capacity_oracle.py <campaign>`. Fixtures are copied
from the hash-verified prior `pkg-namespace-kernel-20261021-140820` snapshots;
originals are unchanged. Future capacity runner invocations also call the oracle.
No new write/flush/sector-cut or BIOS/UEFI acceptance is asserted for this step.

Writer candidate: `pkg-writer-lifetime-20261021-191757-340892` **ASan/UBSan PASS**,
128 threaded exit/in-flight/drop/exec cycles (258 actual registry owner births,
including the same-numeric-PID stale-key check). Actual process records and
credential publication are used; owner objects are a separate bounded private
arena, not retained TCBs. Child-context misuse, stale session/completion, deferred
slot reuse, privilege-drop denial with cleanup allowed, and durable-IDLE=false
admission refusal pass. Host barriers retain work through registry exit/forget;
last completion drops the old session reference. Registry APIs are never called
under the owner guard. Durable IDLE and birth/death/exec notifications are host
adapters; no real filesystem operation or FD lifecycle is integrated into this
test. TSan, IRQ/SMP scheduling, task-exit hook ordering and atomic birth
publication remain separate acceptance requirements.

**Source preservation is separate:** all four final manifests report unchanged
compiled workspace copies and unchanged repository compiled inputs. Other
concurrent AGENTS.md, network, USB, RTL8153 and WLAN work stays unstaged by this
checkpoint. No whole-checkout/boot/release-media preservation claim is substituted
for these compiled-input guards. Aggregate manifest:
`pkg-capacity-owner-summary-20261021-192607/manifest.json`.

Retained attempts:
- `pkg-writer-lifetime-20261021-191621-962678` INCOMPLETE/host assertion FAIL:
  incorrectly treated process_record_exit's waitable-child result as success.
  Corrected the test to verify the actual post-exit registry state; runtime
  assertion/log and original copied source remain.
- `pkg-account-capacity-20261021-191355-128236`, `191707-106315` and
  `191717-353220`: INCOMPLETE, explicitly terminated owned probes while they
  redundantly repeated the expensive whole-filesystem ownership audit at every
  callback. Their manifests retain changed private-input guards after the
  correction; copied inputs stayed unchanged. The final test relies on the
  constructor's existing global audit, then repeats the relevant role/map and
  namespace certificate. No aborted run is credited or deleted.

Next: privately couple this certificate to account-service BEGIN/UPDATE/recovery
and the durable grant, enforce reader exclusion, and connect the owner arena to
trusted birth/abort/exit/reaper and actual grant operations. Account initialization,
pre-replay grant-aware validation and authenticated boot enrollment remain open.
Production integration, installation, trust provisioning, release-media builds,
pushes and physical writes remain disabled.
