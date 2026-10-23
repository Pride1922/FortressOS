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


## 2026-10-21 private account-service, durable-grant and lifecycle coupling

Added a copied-source integrated harness; exact architecture and exclusions are
in [the private integration plan](../plans/PKG2_PRIVATE_SERVICE_INTEGRATION.md).
No shared production C/header or build path is changed by this checkpoint.

Actual `accounts_prepare`, ACCOUNT_BEGIN/UPDATE/FINISH and coherent userdb parsing
now run together with actual candidate grant records, JBD2 allocation/cleanup,
and the capacity certificate in the private translation unit. Headroom uses G
from validated marker/ledger/table records. Existing account readers deny BEGIN;
a successful lease rejects subsequent role-reader opens under actual filesystem
exclusion. A corrupt grant makes UPDATE return the real syscall EIO without
storage writes or account generation change. Valid account updates preserve the
package ledger byte-for-byte, including charged ownership and remaining escrow.

Private callbacks are placed after the copied registry's lock release on commit,
abort, accounted exit and forget. A reserved mapping entry handles detach during
birth registration without nesting locks or retaining a TCB pointer. The runner
extracts actual thread.c FD bodies and syscall.c error mapping, compiles the real
TCB layout, and guards both full source inputs as well as extracted hashes. Real
exclusive-owner dup/dup2 refusal, clone exclusion, CLOEXEC close and descriptor
teardown are used. No new public descriptor or syscall ABI is introduced.

After successful durable acquisition the test admits a second operation, executes
actual accounted process exit and real descriptor close, then lets that retained
operation create/write a tagged object and complete. The old owner references
reach zero while disk state remains RESERVED with T=(8,1), G=(7,0), Q=(1,1).
A fresh writer is refused by that actual disk state. Account operations and
recovery retain this grant. Trusted package cleanup refunds actual storage,
clears ownership and reaches IDLE; a fresh acquisition then succeeds. Its real
CLOEXEC close cancels volatile ownership while preserving the new reservation
until explicit cleanup. Published-process abort and reaper forget callbacks also
execute in the harness.

The account UPDATE interruption is the existing durable empty group-publication
create callback boundary, not an arbitrary sector/journal cut. Two subsequent
fresh processes load retained synthetic expectation context from a separate
artifact file, validate fixed inode/map identities, recover with the actual
account service, retain the grant on the first run and clean it on the second.
The host range/VMM/current-thread and IRQ/scheduler behavior remain adapters.
There is no execution of Ring 3, runqueues, CR3/stack destruction, guest exec,
parallel ordinary writers or production enrollment. The generic constructor still
runs replay before private grant validation: no pre-replay boot policy is claimed.
Global test lane selection/trusted cleanup require serialized host ownership and
are not a production concurrent mutation dispatcher. FD allocation failure after
reservation conservatively retains disk evidence; those unwind branches are
reviewed rather than fault-injected here.

Selected retained-payload audit: `pkg-service-integration-20261021-194247-951528/
retained-payload-audit/manifest.json` PASS. Two extra actual 4096/512 runs retain
both the interrupted and first-recovery images. Linux e2fsck passes on each;
debugfs verifies all 4096 tagged payload bytes remain P, the grant remains
(8,1,7,0,1,1), the charged row remains present, ledger/table hashes match, and the
complete package records are byte-identical across account recovery. This is a
selected geometry audit, not an all-cut or all-geometry payload claim.

Retained earlier attempts:
- `pkg-service-integration-20261021-193741-422537`: strict compile FAIL. The
  generated prefix initialized host feature macros too late, combined host and
  actual TCB headers, and left the reconstruction helper unused. Corrected the
  private generated translation unit to use early POSIX macros, the actual TCB,
  and explicit pre-account-recovery grant reconstruction. Original logs/copies
  remain; no production header was changed to satisfy the harness.
- `pkg-service-integration-20261021-193929-833737`: C assertion FAIL opening
  the tagged file; the inherited lifecycle path still used /mnt/pkg although
  the detached fixture root uses /pkg. Corrected the private path/context and
  added fixed-record inode-generation/map checks. Failure and source remain.
- `pkg-service-integration-20261021-194105-135795`: all four 1 KiB runtime/final
  Linux cases PASS, but original repository compiled-input guard FAIL because
  I added the final parent-callback identity guard in the private reader hook
  while it ran. Copied inputs stayed unchanged; this run is excluded from final
  accepted source-preservation totals.
- `pkg-service-integration-20261021-200637-129285`: incomplete initialization
  directory without a completion manifest; retained and credited with no result.

Installation, production integration/trust provisioning, release-media builds,
pushes and physical disk writes remain disabled. Concurrent networking/USB/WLAN
work and its commits remain preserved. Source/preservation results are separate
from runtime and Linux results; no boot acceptance is asserted.


Final accepted integrated ASan/UBSan results: **12/12 cases PASS** across all six
block/sector geometries, **24 fresh host-process runtime invocations**,
**990 acknowledged account-capacity semantic commits**, **12/12 final Linux
e2fsck/canonical byte/IDLE-record audits PASS**. The selected retained-payload
campaign adds two runtime invocations and two Linux/payload/RESERVED-record
audits, reported separately above rather than counted twice in the matrix.

| Block bytes | Sector bytes | Cases | Accepted campaign |
| --- | --- | --- | --- |
| 1024 | 512/4096 | normal + interrupt/recover each | `pkg-service-integration-20261021-214810-290131` |
| 2048 | 512/4096 | same | `pkg-service-integration-20261021-194237-729483` |
| 4096 | 512/4096 | same | `pkg-service-integration-20261021-194247-951528` |

Invocations: WSL `python3 scripts/test_pkg_service_integration.py --block N` for
1024/2048/4096 (equivalent to standalone `test-pkg-service-integration`). Each
normal mode is one process; each interrupted mode is followed by `resume-keep`
and `resume-clean`. Exact generated C, overlays, extracted FD bytes, binaries,
logs, copied sources, expectation-context files and image/record dumps remain
in each campaign. **Preservation separately PASS:** all three final compiled
workspace-copy and repository-input guards report no changes, including the
full `thread.c` and `syscall.c` inputs used for extraction. Private integration
include hashes agree across campaigns. The original fixture image hashes match.
Concurrent source/worktree changes and commits are outside these per-campaign
claims and remain preserved. Aggregate evidence and retained driver:
`pkg-service-integration-summary-20261021-215518/manifest.json`.

Next private gates are grant-aware read-only validation before the first replay
write, per-operation mutation context for competing kernel callers, and eventual
Ring 3/scheduler lifecycle acceptance. This checkpoint does not enable production
integration or installation.

## 2026-10-22 — Private grant-aware preview before replay

Implemented the copied-source constructor gate described in
[PKG2_GRANT_PREREPLAY.md](../plans/PKG2_GRANT_PREREPLAY.md). The new standalone
runner inserts a grant check after the existing read-only filesystem preview,
before `jbd2_replay`, activation, orphan cleanup or publication. It passes the
explicit preview engine without fabricating `fs->engine`, trust or mount flags.
Exclusive preview reads run unlocked, as required by the actual JBD2 preview API;
checks against published mounts retain filesystem exclusion. The repository's
production EXT4/JBD2 sources are unchanged.

`python3 scripts/test_pkg_grant_prereplay.py` (WSL Ubuntu-24.04), accepted campaign
`.codex-remote-attachments/pkg-grant-prereplay-20261022-172038/manifest.json`:

| Block bytes | Sector bytes | Replay cuts | Restart mounts | Zero-write rejections | Linux audits |
| --- | --- | --- | --- | --- | --- |
| 1024 | 512 | 480 | 960 | 14 | 2 |
| 1024 | 4096 | 256 | 512 | 14 | 2 |
| 2048 | 512 | 672 | 1344 | 14 | 2 |
| 2048 | 4096 | 192 | 384 | 14 | 2 |
| 4096 | 512 | 1056 | 2112 | 14 | 2 |
| 4096 | 4096 | 160 | 320 | 14 | 2 |

**6/6 geometries, 2,816 cuts, 5,632 restart mounts, 84 rejection cases,
12 explicit valid pending-transaction mounts and 12 Linux e2fsck audits PASS**
under ASan/UBSan. Rejections comprise six grant-preview failures and one earlier
filesystem-preview failure for each charge/refund/geometry. External marker hash,
filesystem UUID, ledger inode generation, marker inode generation and profile
mismatch each fail the grant check. A modified on-disk marker hash also fails
there; a raw inode-owner byte alteration without resealing its inode checksum is
correctly rejected by the earlier generic filesystem preview. That last case
proves checksum rejection, not a grant-specific ownership decision. Every rejection
asserts zero write/flush callbacks, unchanged fixture bytes, no mount publication
and released constructor ownership. Wrong external expectations are restored only
in the test context; corrupted target fixtures are discarded, never repaired by
admission.

Real charge/refund transactions are interrupted after durable commit and before
checkpoint. Their preview must validate the journal-updated ledger/table, inode
and allocation state while home images can be older. Subsequent replay cuts use
all four existing persistence modes, before/after callback failure and two restart
mounts per cut. Runtime checks assert exact sequence and G/Q outcomes. Linux
checks here are `e2fsck -fn`; this runner does not claim an independent Linux
byte oracle for every intermediate cut. No account, IDLE or charged-orphan cut
sweep follows from this one-linked-payload RESERVED profile.

**Preservation separately PASS:** compiled copied inputs and recorded repository
inputs, including AGENTS.md and README.md, unchanged through the accepted run;
source fixture image hashes unchanged. This is host recovery acceptance, not boot
or release-media acceptance.

Retained preliminary runs:

- `pkg-grant-prereplay-20261022-171502`: assertion requiring filesystem exclusion
  in the newly added post-mount positive validator. Fixed that published-mount
  call to take the lock.
- `pkg-grant-prereplay-20261022-171552`: the first correction also locked preview;
  actual `jbd2_preview_read_sector` requires unlocked exclusive construction.
  A GDB trace identified that distinct contract. Preview now stays unlocked.
- `pkg-grant-prereplay-20261022-171701`: initial five-negative 1 KiB/512 and
  1 KiB/4096 gate PASS (736 cuts), superseded by the expanded full campaign.
- `pkg-grant-prereplay-20261022-171859`: test expected a corrupted inode checksum
  to reach the grant stage. The generic preview rejected it earlier; corrected
  the stage/call-count expectation and retained the failed run.

The service runner additionally supports `--pre-replay`: its external expectation
context is read before root preparation, and the copied constructor checks fixed
record ownership/maps, namespace bindings, lifecycle grant coherence and the
ordinary account floor. Wrong-marker admission is exercised in a separate process
before actual account recovery, with unchanged image hashes required. Baseline
synthetic enrollment remains a separate known-unenrolled setup path.

Open boundaries: account-capacity certification itself still occurs after replay,
although before account recovery; authenticated enrollment/boot selection, IDLE
restart and charged-orphan admission coverage, per-operation concurrent dispatch,
and Ring 3/scheduler lifecycle acceptance remain open. Installation, production
integration/trust provisioning, release-media builds, pushes and physical disk
writes remain disabled.

Integrated preview review also added an explicit comparison of the actual
filesystem UUID with the external binding, and a fail-closed refusal when the
constructor reports pending orphan cleanup: this service overlay does not yet
couple generic constructor orphan reclamation to grant refunds. That refusal
branch is reviewed, not injected by this campaign. Fresh-process IDLE preview
acceptance and a zero-flush assertion on wrong-marker rejection were added.

Superseded service campaigns are retained, separately from final acceptance:
`pkg-service-integration-20261022-171957-927766` (initial 1 KiB runtime and guards
PASS), `...-172136-473851` (initial 4 KiB runtime and guards PASS), and
`...-172136-471509` (initial 2 KiB runtime/Linux cases PASS but repository-input
preservation FAIL for this task's subsequent `pkg_service_integration.inc`
changes). The latter manifest's inherited `result=PASS` denotes runtime; its
nonempty changed-input list and nonzero runner exit disqualify it as a preserved
acceptance run. No guard failure is silently converted to a pass.
`...-172711-104473`, `...-172731-984369` and `...-172731-984528` were explicitly
interrupted after adding the final orphan refusal. Their manifests are INCOMPLETE,
with own source changes recorded; partial runtime cases are not included in the
final totals. No physical or production state was involved.

These host-only runners build no kernel, initramfs or release medium. Their
preservation claims cover the recorded source inputs and disposable source
fixtures; no before/after release-media hash audit was run for this step. They
therefore establish neither new boot acceptance nor a new release-media
preservation result. No production storage path or physical disk was exercised.

Final integrated pre-replay acceptance (`python3
scripts/test_pkg_service_integration.py --pre-replay --block N` in WSL):

| Block bytes | Sectors | Final accepted campaign |
| --- | --- | --- |
| 1024 | 512/4096 | `pkg-service-integration-20261022-172949-433946` |
| 2048 | 512/4096 | `pkg-service-integration-20261022-172949-433697` |
| 4096 | 512/4096 | `pkg-service-integration-20261022-172949-436321` |

**12/12 normal/interrupted service cases PASS**, including **36 fresh host-process
invocations**, **six IDLE preview restarts**, **six wrong-marker admissions with
zero writes AND zero flushes**, **990 acknowledged account-capacity semantic
commits**, and **12 Linux e2fsck/canonical-account-byte/IDLE-record audits PASS**.
The six rejection invocations preserve complete image hashes. Actual package
record inode/generation/maps, filesystem UUID, fixed names, grant G/Q coherence
and the ordinary floor are checked before replay on the restart paths. No service
runtime or Linux result is counted as boot acceptance. The orphan-refusal branch
remains reviewed rather than injected; these fixtures have no pending orphans.

**Final integrated preservation separately PASS:** all three compiled-copy and
repository compiled-input guards are empty; source fixture hashes are unchanged.
The flush instrumentation is present in the retained generated C; its generator
also requires exactly one matching source function before replacement. That added
fail-fast generator assertion changes no generated C or runtime behavior.
Standalone make targets `test-pkg-grant-prereplay` and
`test-pkg-service-prereplay` are available through `tests/pkg_grant_candidate.mk`;
`make -n` confirmed their commands, and both Python drivers passed `py_compile`.
`git diff --check` passed. No release build or media test was run.

Aggregate evidence, checked source/include identity, totals and retained validator:
`.codex-remote-attachments/pkg-grant-prereplay-summary-20261022/manifest.json` and
`validate.py`. These totals exclude all superseded/incomplete campaigns above.
Next: move the actual conditional account-capacity/namespace certificate into
read-only preview, then implement per-operation concurrent mutation admission and
actual Ring 3/scheduler lifecycle acceptance. Authenticated enrollment and complete
pending/state/trust/retirement admission remain separate, disabled production work.

## 2026-10-22 — Account-capacity certificate before replay writes

Implemented [the private account preview candidate](../plans/PKG2_ACCOUNT_PREVIEW.md).
The mounted snapshot and constructor preview now share `capacity_certificate`,
which derives C-A from the actual fixed role set and dense initialized inode
maps, including extent-tree blocks and empty allocated inodes. The preview builds
its own root/ancestor/role/slot certificate using the existing internal namespace
validators. It neither fabricates trusted/detached/mounted flags nor installs
`fs->engine`, namespace anchors or fences. The constructor still owns the preview
exclusively and reads it unlocked; the mounted wrapper retains filesystem
exclusion, reader refusal and its real namespace fence.

`integration_pre_validate` now derives R_acc in that read-only view before
`jbd2_replay`, activation or other target writes. Its output is published only on
success and compared with the actual mounted certificate after replay. Admission
uses actual preview F and durable G, checking F-G>=max(A0,R_acc) in both dimensions.
The earlier private constructor's A0 floor is retained; since A0 exceeds C on the
current geometries, this step adds structural certification before writes rather
than tightening the numerical threshold. Low-space recovery below A0 even when
R_acc fits remains outside this private admission profile; production account
priority remains a review decision, not a new guarantee.

New rejection fixtures exercise wrong canonical mode, an 8193-byte canonical
file and actual certificate workspace OOM after successful grant checks. The mode
and size fixtures pass Linux fsck before admission. The oversized file is an
explicit invalid-policy fixture grown through the real write planner on the
existing inode with a test-only account allocation bypass. No target repair,
trusted flags, forged allocation counters or legitimate account operation is
claimed for fixture generation. Rejection requires the capacity hook, exact
EACCES/ENOMEM, unchanged result output, zero target writes/flushes, no publication,
released constructor ownership and an unchanged complete image hash.

Five local arithmetic cases per successful certificate test exact equality,
+1 grant and F<G independently in both dimensions. The other dimension remains
at its valid boundary. They use the real derived R and the same budget predicate,
but local F/G inputs: these are not near-full filesystem admission tests.

The new pending-journal fixture uses actual ACCOUNT_TEMP_GROUP (role 7,
`/etc/group.publish`), with a copied-source failure after successful JBD2 commit
and before checkpoint. A fresh process verifies one committed nonempty journal,
then passes the constructor preview/replay path. Required blocks stay unchanged
and required inodes drop by one, matching the durable empty create rather than
stale home state. Mounted rederivation must agree. Linux fsck, size=0,
i_blocks=0, links=1 and an independent C-A calculation from canonical i_blocks
corroborate the replayed certificate. This is one selected commit boundary,
not an all-write/flush interruption campaign or an allocating-data journal sweep.

Retained preliminary campaigns (none counted in final acceptance):

- `pkg-service-integration-20261022-174900-981386`: an initial debugfs remove/write
  oversized-file fixture changed the certified directory record geometry. It
  correctly reached capacity preview and rejected EINVAL with zero writes, but
  failed the fixture's expected EACCES assertion. Replaced that fixture with an
  actual-planner grow preserving inode/namespace identity and geometry.
- `pkg-service-integration-20261022-175154-782720` and
  `...-175415-868900`: the actual committed-journal preview produced the correct
  R inode delta; the independent oracle mistakenly queried `group.new` instead
  of role 7's `group.publish`, causing a missing-stat parsing failure. Corrected
  the path. Their manifests remain INCOMPLETE; manual diagnostic invocations on
  the retained copies are not acceptance cases.

The service runner now records new negative/pending results explicitly and marks
any compiled-input guard failure `FAIL_PRESERVATION`, rather than leaving a runtime
PASS label when preservation fails. The final positive/negative runs and Linux
results are reported separately from source preservation and from earlier boot
acceptance. No boot, release-media, production integration or physical disk work
is performed by this host-only step.

Mounted-path maximum-byte regression:
`python3 scripts/test_pkg_account_capacity.py` in WSL,
`pkg-account-capacity-20261022-175729-270434`: **18/18 recover/update/interrupt cases,
30 fresh-process invocations, 18 Linux fsck/exact 8192-byte account-file audits
and 18 independent Linux capacity-potential oracles PASS** across 1/2/4 KiB
blocks x 512/4096 sectors. Compiled-copy and repository compiled-input guards
PASS separately. Its broader repository inventory records concurrent
`src/drivers/iwlwifi.c` changes; those are outside the compiled input set and were
preserved. Other concurrent Wi-Fi and trust-asset edits remain untouched.

Final service/capacity-preview acceptance, WSL
`python3 scripts/test_pkg_service_integration.py --pre-replay --block N`:

| Block bytes | Sectors | Final accepted campaign | Canonical R | Empty-create replay R |
| --- | --- | --- | --- | --- |
| 1024 | 512/4096 | `pkg-service-integration-20261022-175656-257212` | (80,8) | (80,7) |
| 2048 | 512/4096 | `pkg-service-integration-20261022-175656-256194` | (35,8) | (35,7) |
| 4096 | 512/4096 | `pkg-service-integration-20261022-175656-260545` | (17,8) | (17,7) |

**12/12 normal/interrupted service cases PASS**, plus the separately recorded
capacity-preview checks: **72 fresh-process invocations total**, **36 exact
preview/mounted certificate comparisons**, **180 local arithmetic boundary cases**,
**18 capacity-specific zero-write/zero-flush rejections**, **six committed account
journal preview/replay checks with independent Linux C-A corroboration**, and
**990 acknowledged account-capacity semantic commits**. The six existing
wrong-marker zero-write/flush rejections remain separate from those 18 new
capacity-specific cases. **30 Linux fsck audits PASS**: 12 normal/recovered
account images, 12 invalid-account-policy fixtures, six empty-create replay images.
Normal/recovered images also pass exact canonical account bytes and final IDLE
package-record checks; invalid-policy fixtures are never auto-repaired.

**Preservation separately PASS:** all three final service copied compiled-input
and repository compiled-input guards are empty, and original fixture hashes stay
unchanged. The mounted maximum-byte regression adds **762 acknowledged commits
and 1,836 snapshots** in its 30 runs; its 18 Linux audits and 18 independent
capacity oracles are separate from the 30 service/preview Linux audits above.
The shared certificate source hashes match the final checked repository files.
Python compilation and `git diff --check` pass. Concurrent source/asset changes
outside these compiled input sets remain preserved and are not claimed unchanged.

Aggregate exact totals, source identity checks and retained validator:
`.codex-remote-attachments/pkg-account-preview-summary-20261022/manifest.json`
and `validate.py`. All preliminary failures above are excluded. The earlier
2,816-cut grant replay campaign is historical evidence, not rerun or counted as
a new capacity-cut campaign here. No kernel/release-media build, before/after
media hash audit, boot test, production trust provisioning, push or physical disk
write was performed. Changes remain private and uncommitted.

Next private implementation work: per-operation mutation context and concurrent
caller admission. Production integration still needs low-space account-priority
policy, authenticated enrollment, complete pending/state/trust admission and
actual Ring 3/scheduler lifecycle acceptance. The retained A0 floor, selected
empty-create interruption boundary and absent charged-orphan recovery remain
explicit limits of this checkpoint.


### Private per-operation context and concurrent admission (2026-10-22)

Implemented [the private mutation-context candidate](../plans/PKG2_MUTATION_CONTEXT.md)
in test-only sources. An eight-slot generation arena pins immutable mount/kind
views, retains process-owned package operations and reserves one admission for
account priority. Pending account work blocks new pins/admissions while existing
pins drain. Each filesystem call owns its certificate workspace and account
capacity/grant snapshot; the copied dispatcher no longer classifies concurrent
work through the historical global lane switches. Actual filesystem transactions
remain serialized under the existing filesystem lock. No arena/process lock is
held across filesystem work.

The host delivery adapter uses TLS, not a production TCB binding. Coverage is one
prepared ordinary file, one tagged package file and real account control/backend
callbacks. Initial enrollment and writer acquisition remain trusted quiescent
setup. Neither arbitrary namespace mutation nor concurrent writer acquisition is
admitted by this checkpoint. Actual account credentials/token/service-generation
validation remains in account control; the internal account issuer is trusted.

Accepted standalone admission campaign:
`pkg-mutation-admission-20261022-183929-379884`, WSL
`python3 scripts/test_pkg_mutation_admission.py`: **128 rounds with four
simultaneously pinned workers under ASan/UBSan, and another 128 rounds under
TSan, PASS**. Checks include account priority/cancellation, the reserved eighth
slot, stale generations, wrong mounts, quiescent reconfiguration, failed package
admission reference cleanup, actual process-registry owner death and complete
volatile-reference drain. The standalone durable-IDLE input is mocked; actual
grant state is checked separately by the filesystem campaigns below.

Filesystem invocations: `python3 scripts/test_pkg_mutation_context.py --block N`
and `python3 scripts/test_pkg_mutation_context.py --tsan` in WSL.

| Sanitizer | Blocks | Sectors | Accepted campaign |
| --- | --- | --- | --- |
| ASan/UBSan | 1024 | 512/4096 | `pkg-mutation-context-20261022-183805-040493` |
| ASan/UBSan | 2048 | 512/4096 | `pkg-mutation-context-20261022-183805-035477` |
| ASan/UBSan | 4096 | 512/4096 | `pkg-mutation-context-20261022-183521-513945` |
| TSan | 1024/2048/4096 | 512/4096 | `pkg-mutation-context-20261022-183805-035880` |

**Six ASan/UBSan and six TSan actual-code filesystem cases PASS**, each with
four simultaneously pinned writers (two package, two ordinary), queued account
priority and actual account begin/update/finish. A real update pauses after a
successful staging callback with no filesystem lock held: fresh ordinary,
package and account admissions and new canonical readers reject EAGAIN. Cross-kind
account control, absent context and cross-target writes reject before device
writes. Seven additional blocks after two charged package blocks exceed the
eight-block grant while fitting the backend's 32 KiB bound; rejection is EFBIG
with zero device writes. This is a backend test, not a SYS_WRITE boundary claim.

An already admitted package operation completes after actual accounted process
exit and descriptor close detach its owner; new operations reject EACCES and all
volatile references drain. The durable RESERVED grant remains intentionally
retained, T=(8,1), G=(5,0), Q=(3,1); fresh writer acquisition is rejected. This
workload does not retire the package or return it to IDLE.

**Independent Linux results: 12 clean e2fsck audits and 12 exact payload/account/
grant audits PASS.** Package blocks contain P/Q in either serialized order then
Z; ordinary blocks contain N/O in either order. Canonical account bytes, ledger
self-hash, ownership-table hash and exact final T/G/Q are checked independently
after each process exits. These do not constitute a new interruption sweep.

**Preservation separately PASS:** copied compiled dependencies and repository
compiled inputs remain byte-identical in all accepted campaigns; original
namespace fixture hashes remain unchanged. The aggregate validator rechecks
source identities against the current checkout, sanitizer geometry coverage and
exact totals in
`.codex-remote-attachments/pkg-mutation-summary-20261022/validate.py` and
`manifest.json`. Python compilation, standalone Make target expansion and
`git diff --check` pass. Concurrent work is preserved; broader repository/media
immutability is not inferred from compiled-input guards.

Retained preliminary campaigns, excluded from all accepted totals:

- `pkg-mutation-admission-20261022-182252-342657`: strict compile rejected
  misleading indentation in the new admission source; split the statements and
  retain explicit writer-release error handling.
- `pkg-mutation-admission-20261022-182835-154466`: ASan/UBSan runtime passed;
  bare TSan exited 66 before running tests with `unexpected memory mapping`.
  The same binary subsequently passed with per-process `setarch x86_64 -R`.
  Final TSan runners use that setting; no global ASLR setting changed.
- `pkg-mutation-context-20261022-183011-930756`: strict compile found an unused
  historical capacity dispatcher after replacing it. Keep an explicit reference
  to the compiled baseline helper without dispatching to it or disabling warnings.
- `pkg-mutation-context-20261022-183242-811524`: overspend fixture attempted
  package admission while account priority was pending. Correct EAGAIN triggered
  the fixture assertion. Move the overspend check after the queued account call
  drains so it actually reaches grant spending. No budget behavior was relaxed.

These preliminary manifests remain INCOMPLETE; the earlier successful admission
campaign `pkg-mutation-admission-20261022-183242-733340` is superseded by the final
runner/preservation-label run, not double-counted.

Remaining work includes retained kernel call/TCB binding and cancellation,
general namespace operation coverage, authenticated enrollment and complete
pending/state/trust admission. Account priority here prevents new contenders;
it does not solve a permanently pinned caller, heap/device/journal liveness,
near-full recovery policy or charged-orphan recovery. No new Ring 3, scheduler,
IRQ/SMP, boot or media acceptance is claimed. No kernel/release-media build,
before/after media hash audit, production integration, installation, production
trust provisioning, commit, push or physical disk write was performed. All
changes remain uncommitted.


### Private retained TCB call, cancellation and namespace coverage (2026-10-22)

Implemented the [retained-call candidate](../plans/PKG2_RETAINED_CALL.md) in
test-only sources and exact copied-source overlays. Production `thread.h` and
filesystem code remain unchanged. The copied TCB owns a retained admission and
kernel context; remote cancellation uses PID values and never a remote TCB
pointer. Host TLS supplies only `thread_current()`. Queued account priority is
owned before pinning and released on owner acknowledgement or entry-workspace
OOM. Duplicate/current-task/dead-identity checks, rejection of already pinned
transfer and a post-registration liveness check fence binding creation.

An atomic cancellation/commit gate executes under filesystem exclusion without
taking a binding lock. Cancellation before commit returns EINTR with no device
writes. A commit that acquired the gate first may finish; the next commit rejects.
No record or TCB context is freed during a commit. The copied actual registry's
abort/forget/accounted-exit notifications run after unlocking and publish
cancellation. Owner unwind clears the TCB binding and drains retained admission
and package references. This is a private cooperative acknowledgement protocol;
production scheduler/thread-exit/reaper/exec and signal-delivery wiring is not
claimed complete.

Current-source standalone campaigns, WSL:

- `python3 scripts/test_pkg_call_binding.py`,
  `pkg-call-binding-20261022-190245-207768`: **128 rounds under ASan/UBSan and
  128 under TSan PASS**. Covers owned queued-account cancellation on actual
  accounted exit, cancellation before and during held commit, rejection of end
  during commit, copied foreign-task binding, nested/pinned-transfer rejection,
  stale admission keys and complete arena drainage. During-commit ordering is a
  pthread/atomic protocol test, not an interrupted filesystem I/O test.
- `python3 scripts/test_pkg_mutation_admission.py`,
  `pkg-mutation-admission-20261022-190245-202959`: **128 four-worker rounds per
  sanitizer PASS**, the existing admission/reference/priority regression after
  adding the checked unpinned `pm_describe` snapshot.

Current-source filesystem acceptance uses
`python3 scripts/test_pkg_mutation_context.py --binding` and
`python3 scripts/test_pkg_mutation_context.py --binding --tsan`:
`pkg-mutation-context-20261022-190245-276736` and
`pkg-mutation-context-20261022-190245-276889` (the manifests identify sanitizer).
**Six ASan/UBSan and six TSan cases PASS**, each covering 1/2/4 KiB blocks x
512/4096-byte sectors. TSan uses per-process `setarch x86_64 -R`, as in the
previous checkpoint, without changing global ASLR settings.

Every case retains the prior four-writer package/ordinary and real account
begin/update/finish workload, account priority, paused-account reader exclusion
and admitted package completion after owner exit. It adds actual credentialed
VFS create, mkdir, write, truncate-to-zero, file rename, directory relocation,
unlink and rmdir in a prepared sandbox. Two more simultaneously pinned namespace
callers create/write separate files with account priority queued. This is a
second concurrent phase, not six simultaneous filesystem writers. **22 namespace
semantic commits per case, 264 total**, are acknowledged separately from the
earlier account/package commits.

Real create/mkdir and package-write planners reject cancellation before commit
with EINTR and zero device writes. Actual accounted exit cancels an owned ordinary
create; the owner unwinds before its test TCB disappears. Workspace OOM releases
queued account priority. Destination collision rejects EEXIST without writes;
missing context, account/package paths and dot-component escapes reject without
writes. These finite trusted routes validate actual parent ancestry/mount
identity and retain sandbox nodes. They are not a generic untrusted pathname
confinement API; outside sandbox relocation, symlinks, hard-links and external
imports are excluded.

Truncate exposed a private coverage defect: blanket rejection of every orphan
blocked real cleanup. The overlay now accepts only one transient ordinary orphan
matching the current operation's real captured inode/generation. Account anchors
and role inodes, package records, the charged file and unrelated orphan identities
remain rejected. The shared certificate and production orphan code are unchanged.
Fresh R/G budget checks still enforce F-G >= max(A0,R) under filesystem exclusion.

**Independent Linux results: 12 clean e2fsck audits and 12 exact namespace,
ordinary/package payload, canonical-account and grant audits PASS.** New final
files `/work/b/kept`, `/work/a/parallel0` and `/work/b/parallel1` contain exactly
`namespace-data\n`. Renamed/removed/cancelled and forbidden names are absent.
The package remains intentionally RESERVED at T=(8,1), G=(5,0), Q=(3,1), with
validated ownership-table and ledger hashes; no new IDLE retirement is claimed.

**Preservation separately PASS:** final copied compiled dependencies and repository
compiled inputs remain unchanged, and original fixture hashes are retained.
The aggregate validator rechecks current source identities, six geometries per
sanitizer, namespace commit counts and exact totals:
`.codex-remote-attachments/pkg-retained-call-summary-20261022/validate.py` and
`manifest.json`. Python compilation, standalone Make target expansion and
`git diff --check` pass. Concurrent Wi-Fi/trust-asset work was preserved; the
compiled-input guard is not a whole-repository or media immutability claim.

Retained preliminary evidence, excluded from final totals:

- `pkg-mutation-context-20261022-185246-365525`: the first namespace assertion
  lacked operation diagnostics; retained copy shows a runtime assertion, manifest
  INCOMPLETE. Added precise operation/path/result reporting.
- `pkg-mutation-context-20261022-185528-583312`: actual truncate returned EIO
  because the private certificate rejected its transient orphan. This also
  records `FAIL_PRESERVATION` for integration-source changes while the diagnostic
  run was active. Runtime failure and preservation failure remain distinct.
- `pkg-mutation-context-20261022-185818-225055`: both 4 KiB cases passed before
  the final already-pinned transfer guard was added. Superseded, not counted.
- `pkg-mutation-context-20261022-190028-050078`, `...-053605` and `...-050205`:
  runtime/oracle results belong to their frozen preliminary copies. Their final
  `FAIL_PRESERVATION` labels retain the later `pm_describe` source/header change;
  none contributes to current-source acceptance.
- Earlier binding campaigns `pkg-call-binding-20261022-185100-438650`,
  `...-185509-125092`, `...-190027-998282`, and admission regression
  `pkg-mutation-admission-20261022-190027-985127` passed their then-current tests
  but precede final queued/pinned-transfer regression coverage. Superseded and
  not double-counted.

Remaining work: cancellation/recovery at actual durable multi-commit boundaries,
full private kernel signal/scheduler/teardown delivery and broader adversarial
namespace admission. A permanently stalled caller/device cannot be forcibly
freed; cancellation after an earlier durable intent can require restart/recovery.
Authenticated enrollment, complete pending/state/trust admission, low-space
recovery policy and charged-orphan recovery remain open. No new Ring 3,
scheduler/IRQ/SMP, boot or media acceptance is claimed. Production integration,
installation and production trust provisioning remain disabled. No release-media
build, media hash audit, commit, push or physical disk write was performed.
Changes remain uncommitted.


### Cancellation recovery and real kernel lifecycle validation (2026-10-22)

Implemented [the private cancellation/recovery workbench](../plans/PKG2_CANCEL_RECOVERY.md).
The candidate gains an atomic current-owned cancellation publication. Exact
copied-source overlays exercise VFS operations after durable progress, then
validate and recover the retained ordinary orphan in independent processes.
Production integration, installation and production trust provisioning stay off;
all changes remain uncommitted.

Host acceptance, WSL `python3 scripts/test_pkg_cancel_recovery.py --block N`:

| Sanitizer | Blocks | Sectors | Accepted campaign |
| --- | --- | --- | --- |
| ASan/UBSan | 1024 | 512/4096 | `pkg-mutation-context-20261022-192331-077428` |
| ASan/UBSan | 2048 | 512/4096 | `pkg-mutation-context-20261022-192331-079317` |
| ASan/UBSan | 4096 | 512/4096 | `pkg-mutation-context-20261022-192331-108476` |
| TSan | 4096 | 512/4096 | `pkg-mutation-context-20261022-192331-164998` |

TSan invocation adds `--tsan`; only its 4 KiB geometries are newly covered here,
with the established per-process `setarch x86_64 -R` setting. Each geometry
measures real 70-block truncate and unlink: five acknowledged semantic commits
(intent, three reclamation batches, chain removal). Cancellation after commits
1/2/3/4 must stop the next semantic commit with EINTR and no subsequent device
writes, retain one actual ordinary orphan and drain all call references.

**48 ASan/UBSan and 16 TSan cancellation boundaries PASS: 64 total.** Each gets
a wrong-generation pre-replay rejection with unchanged image hash and zero
writes/flushes, then **two fresh-process recoveries: 128 total**. Every restart
runs the actual allocation/reference audit, grant/namespace/ownership preflight
and account-capacity certificate before replay or cleanup writes. Only the known
ordinary inode/generation is admitted; fixed package records and the charged
file remain excluded. Expected ordinary origin comes from a trusted fixture
sidecar captured before cancellation, not authenticated production enrollment.
Linked shrink intents must retain their sandbox name; deleted intents must not.

**280 fresh host process invocations** include eight seed preparations, sixteen
uninterrupted measurements, 64 cancellation processes, 64 identity rejections and
128 recovery processes. **144 Linux fsck/exact-content audits PASS:** sixteen
normal outcomes and two audits per recovered cut. Account bytes stay unchanged,
the package contains exactly three K blocks, and the ordinary name is either an
empty file after truncate or absent after unlink. A separate independent Linux
oracle checks **80 final images** (64 recovered cuts and sixteen measurements):
exact T=(8,1), G=(5,0), Q=(3,1), ledger/table hashes, and actual tagged inode
identity, generation, root ownership, mode, links, size and i_blocks. Those image
hashes remain unchanged by the oracle. No grant retirement to IDLE occurs.

These are successful semantic/checkpoint-boundary interruptions, not cuts within
sector writes, commit records, checkpoint or flush. Host fflush acknowledges the
fixture's barrier model; no physical power-loss durability follows. No account
transaction/orphan cancellation or general unknown-orphan policy is accepted.

**Separate actual kernel acceptance:**
`python3 scripts/test_pkg_call_kernel.py`,
`pkg-call-kernel-20261022-192112-235513`: **BIOS/UEFI x SMP=1/4, 4/4 private
disk-free boots PASS**. The actual kernel is built only in its copied workspace;
the private test ISO is assembled independently from the copied kernel and a
copy of the existing initramfs with one test program. No release ISO/raw-image
target is built. QEMU has only optical media and, for UEFI, paired pflash code/
disposable vars. No network or data disk is attached.

The Ring 3 program exercises actual signal publication, interrupt observation,
SIGSTOP/CONT parking/resume, caught SIGINT handler/signal return, default
SIGTERM/SIGKILL termination, `process_exit`, `thread_exit`, context switch and
`sched_reap_dead`. Per boot, three rounds require **33 children/reaps and 30
exit-hook binding cleanups**. The caught-handler cases unwind before handler
delivery. Queued account exit releases priority; self SIGKILL while the commit
gate is held cannot free it until the gate is released. Four simultaneous bound
continuations are killed and drained. Exit cleanup precedes descriptor/CR3
teardown, and the actual reaper asserts clear bindings before resource release
and counts after `kfree(dead)`. Subsequent rounds prove reuse.

**Kernel totals: 132 children/reaps and 120 exit-hook cleanups PASS**, with zero
remaining bindings/admissions each round. Observed bound CPU masks are **1 for
both SMP=1 boots and 15 for both SMP=4 boots**, proving execution on CPUs 0-3.
Shell exit status is checked independently of the PASS marker; every VM shuts
down cleanly. This kernel fixture has no package filesystem/grant I/O; its held
commit is an atomic protocol exercise. It does not establish combined kernel
grant/filesystem cancellation, package lease teardown, exec replacement, complete
PMM balance or production integration.

**Preservation is separate from boot acceptance.** The kernel campaign's copied
source, repository inventory and recorded shared-media guards PASS at completion.
The aggregate also derives the actual kernel compiler dependency closure from
its retained dependency files and reconciles it with current sources, private
derived translation unit, assembler/linker inputs and unchanged private ISO.
A later host recovery validator edit is outside that kernel compiled closure;
it is recorded explicitly rather than hidden or counted as a boot failure.
The shared root ELF has no private call-test symbols. Host compiled-copy and
repository compiled-input guards PASS in all final recovery campaigns, and
original fixture hashes remain unchanged. Recorded shared media hashes still
match. These are not whole-repository immutability or physical-media claims.

Current-source regressions also PASS: binding campaign
`pkg-call-binding-20261022-192641-851216` and admission campaign
`pkg-mutation-admission-20261022-192641-841434`, each **128 rounds per sanitizer**.
The admission regression uses four concurrent workers. Python compilation,
standalone Make target expansion and `git diff --check` pass. Exact totals,
current-source reconciliation and the independent record/inode oracle are kept
in `.codex-remote-attachments/pkg-cancel-summary-20261022/manifest.json`,
`validate.py` and `record-oracle/`.

Retained preliminary recovery campaigns, excluded from final totals:

- `pkg-mutation-context-20261022-191538-523677`: strict compile rejected use of
  undeclared `strchr` because the project string interface does not expose it;
  replaced it with a simple mode-string scan. No runtime acceptance.
- `pkg-mutation-context-20261022-191659-961885`: the resumed process had not
  reacquired its retained sandbox node references, so the namespace wrapper
  rejected the normal operation. Restore owned real sandbox references before
  resumed VFS operations; no path policy was relaxed.
- `pkg-mutation-context-20261022-191944-366492`: four truncate cuts passed, then
  deleted-orphan preview rejected at `grant-preview`. Its absent-name check
  expected ENOENT, but the internal directory scanner returns 0 with inode=0.
  Corrected that expectation; no inode, allocation counter or trust flag was
  fabricated. The manifest remains INCOMPLETE with those four partial cases.

Next private work: account transaction cancellation/recovery and combined real
kernel grant/filesystem lifecycle acceptance. Authenticated enrollment/orphan
origin, full pending/state/trust admission, low-space recovery and charged-orphan
recovery remain open. Concurrent work, including trust-asset edits, is preserved.
No production source integration, installation, production trust provisioning,
release-media build, commit, push or physical disk write was performed.

### Account callback cancellation and combined kernel grant/filesystem tests (2026-10-22)

The previous checkpoint is committed as `a12970e`. Implemented the subsequent
[private workbench](../plans/PKG2_ACCOUNT_CANCEL_KERNEL_FS.md); these new changes
remain uncommitted. Production integration, installation and production trust
provisioning remain disabled.

Actual account-service BEGIN/UPDATE/FINISH and redo run with the existing durable
package grant T=(8,1), G=(5,0), Q=(3,1). The fixture measures **18 completed
mutating callbacks** per uninterrupted update. Cancellation after each such
callback causes the subsequent mutating callback or sync to return EINTR before
more device writes. Actual failed UPDATE disables service; CHECK returns EAGAIN
while its writer remains. Every call unwinds its retained binding/admission.

Run in WSL: `python3 scripts/test_pkg_cancel_recovery.py --account --block N`;
the TSan run adds `--tsan` and retains per-process `setarch x86_64 -R`.

| Sanitizer | Block size | Sector sizes | Campaign |
| --- | --- | --- | --- |
| ASan/UBSan | 4096 | 512/4096 | `pkg-mutation-context-20261022-200327-032594` |
| ASan/UBSan | 1024 | 512/4096 | `pkg-mutation-context-20261022-200922-858274` |
| ASan/UBSan | 2048 | 512/4096 | `pkg-mutation-context-20261022-200922-858762` |
| TSan | 4096 | 512/4096 | `pkg-mutation-context-20261022-201439-673911` |

**144 cancellation cuts, 144 wrong-marker zero-write/flush rejections and 288
fresh-process recoveries PASS.** The rejection retains the whole-image hash.
Every successful restart uses actual constructor allocation/reference auditing,
grant/namespace/identity validation and the account-capacity certificate before
replay/activation writes. The first four callback cuts per geometry precede
durable pending publication and recover the old accounts; the other fourteen
recover the complete new accounts. That is **32 old-set and 112 new-set cuts**.
Stages/intent persist when required; fresh account-service recovery performs
the real cleanup and is repeated in a second process.

**592 host probe process invocations** comprise eight seed preparations, eight normal
measurements and four processes per cut (cancellation, rejection, two recoveries).
**296 Linux fsck/exact-content audits PASS**: eight normal outputs and two per
cut. Independent additional checks cover **152 distinct final images** (eight
normal outputs and 144 recovered cuts): ledger/table self-hashes and linkage,
exact T/G/Q, tagged inode/generation/mode/root ownership/link count/actual i_blocks,
exact package payload, canonical account modes/owners/links, and the unrelated
70-block file's complete unchanged contents. Those read-only checks preserve
the image hashes. TSan is newly run only for 4 KiB blocks and this account
campaign is sequential; it does not establish concurrent account writers.

These are completed-callback cuts. Each callback finishes its bounded actual
orphan protocol before cancellation; interruption inside an account callback,
unknown/account orphan admission, sector/commit-record/checkpoint/flush cuts and
uncertain device completion remain excluded. Expected identities still use a
trusted private sidecar, not authenticated durable enrollment. The root-only
account text is coherent service input, not the separate maximum-byte synthetic
capacity profile. The passwd GECOS field changes to `root2`; group/shadow content
is unchanged, although the real transaction restages/removes/publishes all three
roles. Simultaneous content changes in all databases and broader account layouts
are not covered here. Regular-file fflush acknowledgements are the host barrier
model; no physical power-loss claim follows.
The raw campaign manifests retain the earlier generic `scope` sentence naming
ordinary-orphan recovery. Their account `kind`, exact overlays, case modes and
compiled source establish the callback scope described here; those campaigns
do not claim account-orphan admission. The runner's description is corrected for
future account campaigns without rewriting the retained evidence.

Review found that identical group/shadow bytes in the geometry matrix could not
detect mixed old/new contents in those roles. Added the actual-service
`--account-all-files --block 4096` variant, campaign
`pkg-mutation-context-20261022-205843-113958` (ASan/UBSan, 4 KiB blocks/512-byte
sectors). All three coherent root databases change: passwd GECOS becomes `root2`,
group adds explicit `root` membership, and shadow changes its locked marker from
`!` to `*`. Every recovery must produce byte-exact old or new values in all roles.
Selected cuts **3/7/8/10/11/13/14/15/18** cover pre-intent staging, each canonical
remove/publication, pending retirement and final staging cleanup. **Nine cuts,
nine zero-write rejections, eighteen fresh recoveries, nineteen Linux audits and
ten independent final-image grant/noninterference audits PASS**. One cut recovers
old bytes; eight recover all-new bytes. This is a focused vector, not another
full geometry matrix or broader account-layout acceptance.

**Aggregate host totals: 153 cuts, 153 zero-write/flush rejections, 306 fresh
recoveries, 630 probe process invocations, 315 Linux audits and 162 independent final-image
grant/noninterference audits PASS.** Old/new results total 33/120 cuts.

**Combined real-kernel acceptance:** WSL
`python3 scripts/test_pkg_call_kernel.py --filesystem`, campaign
`pkg-kernel-fs-20261022-203440-956971`, **BIOS/UEFI x SMP=1/4: 4/4 PASS**.
The kernel builds only in the copied workspace; one private Ring 3 probe and a
copy of the retained 32 MiB IDLE fixture/context enter its independently assembled
test ISO. QEMU has optical media and paired disposable OVMF vars only: no network,
data disk or physical target. All per-boot argv are retained and checked.
The combined kernel filesystem geometry is 4 KiB blocks with 512-byte sectors
only; the host geometry matrix is a separate claim.

Actual privileged registry identity and durable IDLE authorize volatile writer
acquisition and the existing real grant reservation planner. A loader-mapped BSS
RAM device runs the actual detached EXT4 constructor, pre-replay grant/capacity
checks and real EXT4/JBD2 writes. The fixture creates one charged inode and eight
children append one block each, including a four-writer phase. SIGKILL, SIGTERM,
STOP/CONT then TERM and caught SIGINT stop each next real planned write with
EINTR and zero additional writes by that operation. Counter sampling and the
denied write occur under the same filesystem exclusion, so another writer cannot
invalidate the measurement. Caught delivery follows binding unwind; actual
default exit and reaper hooks clear/drain the other bindings.

**32 children/reaps, 28 exit-hook cleanups, 32 cancelled next writes and 36
tagged semantic commits PASS** across the four boots. The 36 count includes four
inode creates plus 32 appends; four durable reservation commits and lifecycle
activation/freeze are separate. Final G=(0,0), Q=(8,1), T=(8,1), still RESERVED.
Closing/detaching all volatile writer references does not clear the grant; a new
begin refuses the non-IDLE state. Every round requires zero bindings/admissions.
Recorded CPU masks demonstrate bound execution on an AP in both SMP=4 boots.
Exact masks are 1 in both SMP=1 boots and 15 in both SMP=4 boots. Polled TLB
acknowledgements are 0/222 for BIOS SMP=1/4 and 0/221 for UEFI SMP=1/4. Each
boot observes 1792 RAM-sector writes and 70 fixture flush acknowledgements;
these are device-callback counts, not physical durability measurements.
Shell exit status is checked independently; every accepted VM shuts down cleanly.

The constructor audits global ownership while unpublished/exclusive. Reservation
rechecks actual roles/inodes/maps under filesystem exclusion without repeating
the quadratic audit there. Final verification freezes the backing, opens a fresh
naturally unpublished engine under retained exclusive device ownership, and runs
the full global audit with zero additional device writes/flushes. No mounted
ownership/trust flag is altered to bypass its lock contract. Four read-only QEMU
virtual-memory exports independently pass Linux fsck, exact package/account bytes,
record hashes and inode/generation/mode/root ownership/link/allocation checks;
export hashes remain unchanged by those checks.

The trusted private dispatcher supplies the coordinator's retained writer values
to child calls. This is not a production user lease/FD ABI or acceptance of
per-caller package authorization, descriptor nontransferability, automatic writer
owner-death/exec session retirement or kernel account-service updates. RAM
barriers are fixture acknowledgements, not controller ordering or reboot/power
durability. No production mount, complete package pending/state/trust protocol,
charged-orphan recovery, low-space progress or installation acceptance follows.

**Preservation is separate from boot/runtime acceptance.** Final host compiled
copies and current compiled repository inputs match. The kernel campaign's
source-copy/repository and shared-media guards pass separately at campaign completion. The aggregate
also reconstructs exact private generated helpers, reverses recorded overlays
against current source, and validates the actual compiler dependency closure,
assembler/linker inputs, fixture/context and private ISO hashes. Shared release
media are unchanged; there is no whole-working-tree immutability claim.
The subsequent all-files host option and manifest-description correction change
`scripts/test_pkg_cancel_recovery.py`;
that later change is outside all **274 actual kernel dependency inputs**. Its old
and current hashes are retained explicitly. No later kernel compiled input changed
and the boot results are not conflated with that host-harness inventory change.
Exact totals and the independent record/noninterference oracle are retained in
`.codex-remote-attachments/pkg-account-kernel-summary-20261022/manifest.json`,
`validate.py` and `grant-oracle/`. Python compilation, standalone Make target
expansion and current `git diff --check` pass.

Retained preliminary kernel campaigns, excluded from final totals:

- `pkg-kernel-fs-20261022-200838-999903`: private overlay forward declarations
  were missing/incorrect; strict build rejected them. No boot acceptance.
- `pkg-kernel-fs-20261022-200955-720560`: the copied grant validator lacked the
  repository-root include path. Strict build rejected it; no boot acceptance.
- `pkg-kernel-fs-20261022-201112-173672`: BIOS/SMP=1 passed its then-current
  fixture; BIOS/SMP=4 halted on a real TLB shootdown ACK timeout and the runner
  was interrupted after retaining the fatal log. Not current-source acceptance.
- `pkg-kernel-fs-20261022-201608-634858`: loader-mapped backing avoided runtime
  allocation of that backing, but BIOS/SMP=4 still hit the ACK timeout. This
  does not establish an allocator defect. Review found duplicate full ownership
  audits under IRQ-disabled filesystem exclusion in the private workbench.
- `pkg-kernel-fs-20261022-202104-298329`: attempting the final audit on the live
  mounted engine without its lock correctly triggered the real ownership
  assertion. The final fixture uses a separate naturally unpublished audit
  engine over frozen backing; it does not disable lock assertions or fake flags.
- `pkg-kernel-fs-20261022-202457-518109` still retained a BIOS/SMP=4 TLB timeout.
  Removing duplicate locked audits did not fully solve mailbox progress.
- `pkg-kernel-fs-20261022-203059-563667` added phase markers and localized the
  timeout to the long constructor, before namespace admission or any package
  writer. Syscall IF remained masked even when no lock was held. The final RAM
  adapter services the existing lockless TLB polling protocol at every sector/
  flush callback, records serviced acknowledgements, and changes no production
  timeout, interrupt window or lock assertion.

Next design/integration work includes authenticated enrollment/orphan provenance,
complete pending/state/trust admission, account interruption inside callbacks,
charged-orphan and low-space recovery, actual per-caller lease/lifecycle binding
and broader end-to-end acceptance. Concurrent work remains preserved. No release
media target, installation, production trust provisioning, commit, push or
physical disk write was performed in this continuation.

### Private mid-callback account-orphan recovery (2026-10-23)

Continuation of `2e911f9`; this continuation remains uncommitted. Contract and
scope: [account-orphan recovery](../plans/PKG2_ACCOUNT_ORPHAN_RECOVERY.md).
Production integration, installation and production trust provisioning remain
disabled. No release-media target, push or physical/raw disk write was used.

The private constructor now admits a bounded account orphan only against a
trusted fixture origin captured while the actual fixed-role name was still
linked. UUID, parent/role, inode/generation, root owner/group, exact mode, size,
zero links, role vacancy, original logical/physical extent prefix and remaining
physical allocation/i_blocks must agree. Package identities and the actual
charged inode are excluded. The complete constructor allocation/reference audit
still precedes this policy and replay. No mounted/trust flags, allocation
counters or account identities are fabricated. Unknown origins remain rejected.

Capacity is `R=C-A_linked-A_orphan`, counting actual remaining orphan data/tree
blocks and its still-allocated inode. It changes only with actual allocation
counter deltas. Each stranded callback independently compares this certificate
with its retained per-call tracker. Fresh constructor cleanup must increase R
by exactly the blocks/inode actually reclaimed. An orphan with zero data still
occupies one inode. Grant records remain RESERVED T=(8,1), G=(5,0), Q=(3,1).

Every new account file is a coherent, distinct, exactly 8192-byte root-only
database with parser-valid comment padding. This tests maximum file bytes and
multi-block account reclaim, not broader account layouts. Actual semantic
commits are counted, rather than assumed from callback or byte counts.

ASan/UBSan invocations (WSL Ubuntu-24.04):

```
python3 scripts/test_pkg_cancel_recovery.py --account-mid --block 4096 --local-images --jobs 8
python3 scripts/test_pkg_cancel_recovery.py --account-mid --block 1024 --local-images --jobs 8
python3 scripts/test_pkg_cancel_recovery.py --account-mid --block 2048 --local-images --jobs 8
```

| Block / sector bytes | UPDATE cuts | Additional redo cancellations | UPDATE orphan states | Zero-write/flush rejections | Linux audits | Result |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| 1024 / 512 | 39 | 36 | 14 | 133 | 155 | PASS |
| 1024 / 4096 | 39 | 36 | 14 | 133 | 155 | PASS |
| 2048 / 512 | 39 | 36 | 14 | 133 | 155 | PASS |
| 2048 / 4096 | 39 | 36 | 14 | 133 | 155 | PASS |
| 4096 / 512 | 39 | 36 | 14 | 133 | 155 | PASS |
| 4096 / 4096 | 39 | 36 | 14 | 133 | 155 | PASS |

The six geometries cover **234 UPDATE cuts**, **216 additional actual redo
cancellations**, **924 fresh recovery processes**, **798 zero-write/flush
rejections**, **930 Linux audits**, **468 independently audited final images**
and **2196 C probe invocations**. Twelve further redo attempts correctly observe
zero remaining semantic commits after constructor cleanup; they are recorded
as no-ops and excluded from cancellation totals. No work is invented to make
such a test appear interrupted. Two fresh recoveries follow each initial cut
and each repeated-cancellation case. The final UPDATE cut has no repeat branch;
its unused cloned retry image is excluded from audited-image totals.

Marker/identity/generation, missing origin, invalid role, wrong parent/UUID and
allocation-proof negatives reject before writes/flushes. Directed disposable
copies change actual orphan UID, GID and mode through Linux debugfs and must
also reject before writes/flushes. The ordinary package-identity mismatch does
not alone prove the charged-inode guard. A separate fixture starts from actual
UPDATE cut 26, with canonical shadow vacant, and constructs an allocated
charged orphan with matching expected inode/generation, valid allocation and
resealed grant/table hashes. Complete ownership preview succeeds; the explicit
charged-identity branch rejects with zero writes/flushes and unchanged image.
A debugger trace confirms that branch; the debugger run is diagnostic, not an
additional sanitizer or recovery acceptance run.

Focused 4 KiB / 512-byte-sector recovery uses the same compiled candidate and
the exact checked-in transport/Linux oracle functions, retained in
`.codex-remote-attachments/pkg-account-orphan-summary-20261023/focused-recovery.py`.
It cancels every measured recovery commit from three genuine UPDATE checkpoints:

| Initial UPDATE cut / retained role | Recovery cuts | Orphan states | Fresh recoveries | Linux audits | Result |
| --- | ---: | ---: | ---: | ---: | --- |
| 8 / intent.tmp | 12 | 8 | 24 | 25 | PASS |
| 17 / group.publish | 34 | 17 | 68 | 69 | PASS |
| 23 / shadow.publish | 35 | 18 | 70 | 71 | PASS |

This adds **81 cancellations**, **162 fresh recoveries**, **167 zero-write/flush
rejections**, **165 Linux audits**, **84 independent final images** and **416 C
probe invocations**. Reused allocation holes fragment the inline publish-file
extents: group/shadow recovery requires 34/35 commits, not the initially assumed
33/33. Actual partial right-edge states retain one of two originally allocated
data blocks; both capacity conservation and later recovery pass. Across the
matrix and focused campaign, all **11 fixed roles** have directly observed
account-orphan admission/recovery coverage. Roles 7/8/9 have focused 4 KiB/512
coverage; this is not all-role coverage at every geometry or an external extent
tree acceptance claim.

Feature totals, excluding preliminary/debug runs and the older callback-only
regression: **531 injected cancellations**, **1086 fresh recoveries**, **966
zero-write/flush rejections** (including the separate charged-orphan negative),
**1095 Linux audits** and **552 independently audited final images**. The
directed charged fixture has one setup probe and one accepted negative probe;
with those, the feature uses **2614 completed C probe invocations**.

Independent Linux e2fsck/debugfs checks complete old/new account bytes and
root ownership/modes/links; absent intent/staging/publish roles; unchanged package
and unrelated 70-block file bytes; ledger/table self-hashes and linkage; exact
T/G/Q, transaction/sequence, and actual charged inode/generation/size/allocation.
Every audit preserves its whole-image hash. EINTR permits no device writes after
cancellation, owner unwind drains all PC/PM bindings/pins and account
authentication remains disabled until actual redo recovery succeeds.

The prior all-three-database completed-callback regression also passes separately:
`python3 scripts/test_pkg_cancel_recovery.py --account-all-files --block 4096`,
9 cuts, 18 fresh recoveries, 19 Linux audits and 38 C probes. It is excluded from
the feature totals above. No new TSan, kernel signal/scheduler, Ring 3 or boot
acceptance is claimed. The previous combined kernel BIOS/UEFI campaign remains
separate historical evidence.

**Preservation is separate from runtime acceptance.** Final compiled copies and
current compiled repository inputs match. Source fixtures retain their accepted
hashes; parallel cases use distinct images/processes and immutable seeds.
`--local-images` uses disposable Linux regular files and copies back completed or
partial image/sidecar artifacts even on failure. It does not test concurrent
writers on one mount. Shared `bin/` media hashes/key set match the retained
pre-task baseline. Concurrent AGENTS.md, README.md and the existing dirty SSL
hash file are preserved. No whole-working-tree immutability claim is made.

Accepted primary campaigns:

- `pkg-mutation-context-20261023-073659-492097` (4 KiB, both sector sizes).
- `pkg-mutation-context-20261023-073709-760472` (1 KiB, both sector sizes).
- `pkg-mutation-context-20261023-073720-034473` (2 KiB, both sector sizes).
- `pkg-account-orphan-summary-20261023/focused-recovery-v2` (focused redo).
- `pkg-account-orphan-summary-20261023/charged-negative-v4` (directed refusal).
- `pkg-mutation-context-20261023-072106-648795` (separate callback regression).

Aggregate exact hashes, counters, source guards and retained diagnostic artifacts
are in `.codex-remote-attachments/pkg-account-orphan-summary-20261023/manifest.json`
and `validate.py`.

Retained preliminary results, excluded from acceptance totals:

- `pkg-mutation-context-20261023-071009-302965`: strict compilation found use of
  strchr without a project declaration; replaced with an explicit character scan.
- `071122-994477`, `071434-058810`, `071456-283529`, `071506-186944` (same prefix):
  interrupted while strengthening the map witness, maximum-byte input and
  zero-work recovery handling; the captured source-preservation failures remain.
- `071919-047269`, `071929-262240`, `071939-516627`, `072328-173728`,
  `072338-337586`, `072348-670723` (same prefix): interrupted when switching
  regular-file transport and independent-case parallelism. Partial results are
  retained and are not final matrix acceptance.
- `pkg-account-orphan-debug-20261023`: targeted debugging and an O1/O2 timing
  comparison, excluded from acceptance. Ownership auditing dominated CPU time;
  changing optimization did not materially improve it. Final campaigns retain
  all audits and the original sanitizer compilation profile.
- `charged-negative`: a synthetic orphan incorrectly retained clean lifecycle
  markers; the constructor correctly refused before package preflight. The final
  fixture uses a real interrupted checkpoint and dirty/RECOVER state.
- `charged-negative-v2`: narrower successful directed refusal before the vacant
  role refinement; excluded in favor of v4.
- `charged-negative-v3`: the helper checked only debugfs stdout for a missing
  pathname, but the diagnostic was on stderr. Original logs remain; v4 checks
  combined output.
- `focused-recovery`: 12 pre-intent recovery cuts passed, then the baseline's
  actual 34 commits rejected a guessed 33-commit expectation. The final helper
  enumerates measured commits and retains the original failed assertion.

The origin sidecar is trusted native host fixture state, **not authenticated
durable production provenance**. Adopting such a witness from untrusted input
would be unsafe. Unknown, multiple, linked or package-charged orphans remain outside
admission. Cancellation inside constructor cleanup, sector/flush interruption,
uncertain I/O, external extent trees, full account layouts and physical durability
remain separate work; these fixtures do not exercise near-full recovery progress.
Production integration, installation, trust provisioning,
release builds, pushes and physical writes remain disabled.

### Enrollment and durable provenance design checkpoint (2026-10-23)

[Contract and implementation gates](../plans/PKG2_ENROLLMENT_PROVENANCE.md).
Reviewed the current sidecar candidate against actual fixed-role remove and
traditional orphan-chain paths. The sidecar is written after cancellation unwind
and does not establish abrupt-loss provenance. Signed immutable enrollment and
kernel-controlled mutable provenance are distinct authority boundaries; checksums
are not authentication against offline rewriting.

The proposed first profile uses one preallocated protected provenance slot:
activation must share the unlink/orphan-insertion commit; retirement must share
the inode-free/orphan-detachment commit. Partial reclamation retains the original
witness. A durable account-operation generation and its atomic advancement are
newly explicit prerequisites; process IDs and package transaction IDs cannot
stand in for them. Canonical codecs, signature verification, durable planner
hooks and actual JBD2 interruption tests remain implementation gates. This is
a design checkpoint, not completion of remaining work area 1.

No runtime code, fixture authority or previous acceptance evidence changed.
No new host recovery, kernel, Ring 3, boot, media or physical acceptance is claimed.
Production integration, installation, production trust provisioning, release-media
builds, pushes and physical disk writes remain disabled. Changes are uncommitted.

### Private enrollment signature and provenance codec gate (2026-10-23)

Implemented the first gate of the enrollment/provenance design in private
`tests/pkg_enrollment_candidate.{h,c}`, with the checked-in C harness and
`scripts/test_pkg_enrollment.py`. Exact candidate wire layout, API ownership,
authority boundary and limitations: [codec contract](../plans/PKG2_ENROLLMENT_CODEC.md).
No production source, boot parser, installation or trust provisioning changed.

Accepted invocation (WSL Ubuntu-24.04):
`make -f tests/pkg_grant_candidate.mk test-pkg-enrollment`.
Evidence: `.codex-remote-attachments/pkg-enrollment-20261023-071457-970929/`.
Manifest SHA-256:
`db18345dada8a958891572196b93f103b100fb5d88a6a8ccdea222395b3e31d2`.
Probe SHA-256:
`906d7778f0eec097a98cdbda33a82719ac076a6504bca4ac216f9599a6d00bf0`.
Independent vectors SHA-256:
`b6e17d341f1d935bdc93958f9aca735766443c4f9531ca01bf4ec9783adb87bf`.

ASan/UBSan and strict warnings: PASS, 64 enrollment cases, 83 origin cases,
3680 truncation rejections and 3680 single-byte mutation rejections. Python
struct/hashlib and cryptography/OpenSSL independently produce and sign fixtures;
actual unchanged Fortress SHA-256/Ed25519 verifies them. Signed malformed
structures are re-signed and re-pinned; malformed origin records are resealed.
Separate bad-signature/unknown-signer/threshold/pin cases isolate authentication.
The maximum eight-signature/eight-key acceptance, +1 key-count and +1 maximum
wire-length rejections are explicit. All eleven origin roles have positive
1/2/4 KiB profiles. Zero-size allocated files, fragmented data and one metadata
block budget are represented; this is not an actual extent-tree validation test.
Failures preserve writable output; origin canonical re-encoding is byte-exact.

Freestanding GCC -O2 compilation with <=1024-byte candidate frame gate: PASS.
Measured static frames: descriptor 136, signature gate 176, origin decode 208,
origin encode 128, extent helper 8 bytes. This excludes upstream crypto frames
and does not establish a full Ring 3 or kernel call-chain bound.

Input/concurrent-work and existing canonical shared-media hash guards: PASS.
Copied-source preservation: PASS. These are separate preservation results, not
boot or media acceptance. No QEMU, release-media build or physical device access.

Retained preliminary evidence, excluded from final acceptance counts:

- `pkg-enrollment-20261023-070950-517025`: FAIL at isolated compilation because
  the runner lacked the upstream Monocypher include path. Fixed in the runner.
  Python domain padding was also corrected by review before the next run;
  this first run never reached runtime vectors.
- `pkg-enrollment-20261023-071106-342757`: FAIL at origin case 36: the negative
  fixture wrote the existing UUID byte back unchanged. Corrected the value and
  added an explicit no-op mutation guard; no candidate relaxation was required.
- `pkg-enrollment-20261023-071314-858804`: PASS (60 enrollment/83 origin cases,
  same truncation/mutation totals), superseded by the final maximum-signer and
  maximum-length expansion and copied-source artifact guards.

This completes only the private read-only codec/signature gate. The externally
supplied policy and immutable verified-view lifetime remain caller contracts.
The codec does not compare actual filesystem identities/bitmaps, enforce the
namespace fence, validate live pending/trust phase coherence or authenticate
arbitrary mutable/offline rewrites. Independent durable sequence/account-generation
storage is not implemented. Atomic slot activation with unlink, retirement with
inode free, JBD2 cuts and recovery without an origin sidecar remain next.
The prior sidecar recovery evidence is unchanged and no new filesystem recovery,
Ring 3, kernel, boot, hardware or production acceptance is claimed. Production
integration, installation, production trust provisioning, release-media builds,
pushes and physical disk writes remain disabled. Changes remain uncommitted.


### Private durable account-generation and atomic JBD2 origin pair (2026-10-23)

**Runtime PASS; shared-media preservation FAIL, retained separately. This is not
a clean all-gates PASS or new boot acceptance.**

[Storage contract and limitations](../plans/PKG2_ACCOUNT_DURABLE.md).
Implemented `tests/pkg_account_durable.inc` and
`scripts/test_pkg_account_durable.py`, with the standalone private make target
`test-pkg-account-durable`. Production sources remain unedited by this task;
exact copied-source hooks join the actual EXT4 unlink/orphan cleanup planners
and pre-replay constructor preview. No fabricated mount/trust/ownership flags.

A preallocated generation record supplies the expected account-operation
generation and slot sequence to the origin decoder. BEGIN advances generation
and rebinds EMPTY atomically. Unlink activates the original inode/map witness
with the directory removal, zero-link inode and orphan insertion. Partial
reclamation preserves ACTIVE. Final inode free/orphan detachment clears the
slot with the generation record's sequence/digest update. END publishes IDLE
only after EMPTY and no orphan. Both records are JBD2 metadata images in the
same semantic commit, with bounded pre-I/O credit checks and no split fallback.

The fixture exports actual filesystem UUID/geometry and sixteen real inode,
generation and map locators, then signs enrollment externally with deterministic
public test keys through Python cryptography/OpenSSL. Each fresh process uses
the private enrollment verifier and an independent descriptor pin. No `.origin`
sidecar is generated, loaded or used as orphan authority. Mutable checksums
remain corruption detection under exclusive test-dispatch ownership, not
authentication of arbitrary offline changes or complete-root rollback.

Exact accepted runtime invocations in WSL Ubuntu-24.04 (independent campaigns):

```
python3 scripts/test_pkg_account_durable.py --block 1024 --jobs 8
python3 scripts/test_pkg_account_durable.py --block 2048 --jobs 8
python3 scripts/test_pkg_account_durable.py --block 4096 --jobs 8
```

Each process owns a distinct RAM fault disk. Eight persistence/before-after
profiles per geometry cover cached, all, alternating and inverse sector
persistence. Their actual event counts match for BEGIN, pinned unlink,
last-close cleanup, constructor recovery and END. Sector callbacks are atomic;
no actual hardware or torn-sector claim follows.

| Block bytes | Sector bytes | Injected cuts | In-process restart passes | Committed preview newer than home |
| --- | --- | --- | --- | --- |
| 1024 | 512 | 2056 | 4112 | 392 |
| 1024 | 4096 | 1192 | 2384 | 216 |
| 2048 | 512 | 3784 | 7568 | 744 |
| 2048 | 4096 | 1192 | 2384 | 216 |
| 4096 | 512 | 7240 | 14480 | 1448 |
| 4096 | 4096 | 1192 | 2384 | 216 |

Total **16,656 cuts**, **33,312 in-process restart passes**, and **3,232** preview
observations newer than the home generation block. Every cut has two actual
constructor/recovery passes; the second preserves pair sequence/generation.
These are host object teardowns/reopens, not 33,312 fresh OS processes.
Separately **24 fresh-process recoveries** start from ACTIVE and partially
reclaimed snapshots and recover each twice without an origin sidecar.

**432 zero-I/O rejections**: 90 constructor negatives plus 342 direct primitive
guards. Resealed negatives isolate generation/sequence binding, unknown roles,
inode/parent/map mismatches, missing origin, ACTIVE/IDLE incoherence, EMPTY with
an orphan, reserved data, overflow/near-overflow and actual marker digest.
Constructor rejection requires `account-provenance-preview`, no published mount,
released constructor ownership, zero writes/flushes and unchanged image bytes.
Direct tests refuse ordinary overwrites/unlinks of the two records, competing
BEGIN and END while ACTIVE. Six live counter-boundary cases complete the last
admitted operation, then refuse the next BEGIN with EFBIG and no I/O. Counter
exhaustion is terminal admission failure; no counter reset/repair is implemented.

ASan/UBSan and strict -Wall/-Wextra/-Werror: PASS for all runtime cases.
Independent Linux checking: **54 audits** (42 campaign audits plus 12 supplementary
identity/profile audits). e2fsck is read-only; debugfs reads verify exact Python
wire bytes for the pair, inode/generation/root ownership/mode/block count,
absent synthetic shadow and unchanged unrelated payload. All 48 retained profile
final images are either independently audited or byte-identical to an audited
image. Linux does not audit every cut. Image hashes are unchanged by each audit.
No new TSan, freestanding/kernel frame or full call-chain acceptance is claimed
for these host-only storage hooks.

Aggregate reconciliation and supplementary Linux oracle:
`.codex-remote-attachments/pkg-account-durable-summary-20261023/`.
Aggregate manifest SHA-256:
`abaac9a41f40b0a19e02b3475db08fa662d50e2f2329300121abca4cc3ade70d`.
It reports `runtime_status=PASS` and `media_guard_status=FAIL_RETAINED`, records
each raw manifest/hash without modification, and verifies all copied compiled
dependencies plus **47 shared compiled source inputs** against the current tree.

Raw campaign evidence under `.codex-remote-attachments/`:

- `pkg-account-durable-20261023-082546-041238`: raw **FAIL**; manifest SHA-256
  `8eac8f3f08e796c5aee5d7930fc66d1b0508fada4b8021c4c7741a3ac3bed25d`; probe SHA-256
  `0f91e61053eb897728f39564a07b98ecb1d5afbd7a2960b68f1a109c0c5f22e3`.
- `pkg-account-durable-20261023-082546-053606`: raw **FAIL**; manifest SHA-256
  `870421c6ac924da99d3dafcca650fde4a15a47899530539b9204af35174791c3`; probe SHA-256
  `335b93f9394f18b34fc722cbbeb5499f9a61559f71e6d485e1bdd1078b9e8b51`.
- `pkg-account-durable-20261023-082528-088173`: raw **FAIL**; manifest SHA-256
  `24985645d5a5b177b2beccdc308cab169865d92f8d5b99bfbb34562b76665353`; probe SHA-256
  `d65559aa858e06610345f2847c5fae965b9cdcd458c9d6bca6b90ced23304739`.

The three raw statuses remain **FAIL** because their canonical output guards
changed: `bin/fortress.elf`, `bin/initramfs.tar`, `bin/fortress.iso` and
`bin/fortress.img`. Concurrent changes also appeared in `src/drivers/iwlwifi.h`,
`src/net/wlan/net_80211.c`, `src/net/wlan/wlan_mgr.c` and
`src/net/wlan/wlan_mgr.h`; none is a compiled dependency of this gate. No copied
input or compiled repository dependency changed. AGENTS.md, README.md and the
preexisting dirty SSL digest asset retain their guarded hashes. This separates
qualified source preservation, failed shared-media preservation and runtime
evidence. No canonical-media preservation/exclusion acceptance is claimed.
This task's build/fixture/audit commands target only private copied-source
workspaces and disposable regular files; it executed no release build, push,
installation, production trust provisioning or physical disk write. Concurrent
source/output changes were left intact rather than restored or incorporated.

Retained preliminary evidence, excluded from the totals above:

- `pkg-account-durable-20261023-080918-844872`: FAIL, isolated harness compilation
  lacked TEST_PERMISSIONS_ENFORCEMENT required by existing stage-write code.
  Fixed the private generated harness configuration; no production source edit.
- `pkg-account-durable-20261023-081119-184443`: stopped while switching the full
  sequential cut sweep to independent profile processes. Raw FAIL also retains
  the unrelated concurrent iwlwifi header preservation failure. Partial images
  remain; no completion claim.
- `pkg-account-durable-20261023-082124-005456`: INCOMPLETE, stopped before final
  acceptance to add the mounted counter-exhaustion boundary test. Partial logs
  and ACTIVE/partial images remain. Final campaigns use the same frozen final
  candidate and include that directed boundary case.

The scope is one synthetic canonical shadow removal, pre-intent phase, <=8192
bytes. Its S-filled payload is not a valid account database. This durable
operation generation does not replace credential snapshot/authentication
generation. Actual account-service BEGIN/UPDATE/FINISH and redo-phase coupling,
all-role recovery, pending/state/trust admission, complete ordinary namespace
fencing, allocation/pool/credit exhaustion injection, low-space progress,
charged-orphan refunds, uncertain/torn I/O, kernel lifecycle/signal/scheduler
coupling and physical durability remain open. The earlier sidecar campaign is
unchanged. No new Ring 3, QEMU, boot, release-media or physical acceptance.
Production integration and installation remain disabled. Changes are uncommitted.


### Private durable account-service and redo integration (2026-10-23)

Implemented the [private service layer](../plans/PKG2_ACCOUNT_SERVICE_DURABLE.md)
using copied-source overlays only. Actual account BEGIN/UPDATE/FINISH, detached
namespace admission and redo now join the generation/origin pair. Version 2 keeps
credential generation separate from operation generation, persists the complete
planned intent and old database hashes before staging, and couples pending
publication/removal to the appropriate JBD2 phase update. Actual orphan cleanup
covers all eleven fixed roles. Admission stays disabled after cancellation or
process abandonment until fresh validation/recovery closes the durable operation.
Production integration and installation remain disabled; changes are uncommitted.

Runtime results under ASan/UBSan and strict compiler warnings:

| Campaign | Exact coverage | Result |
| --- | --- | --- |
| Service matrix | 1/2/4 KiB x sectors 512/4096; 42 UPDATE commits each; 252 UPDATE cuts and 252 first-recovery-commit cuts | PASS |
| Directed redo | 4 KiB x sectors 512/4096; UPDATE seeds 9/18/24 retain intent.tmp/group.publish/shadow.publish; 15/37/38 recovery commits per sector size, 180 cuts total | PASS |
| Pending-publication I/O | 4 KiB x sectors 512/4096; 141/22 callbacks respectively; cached/all/alternating/inverse persistence x before/after = 8 profiles; 1304 callback cuts | PASS |
| Preview rejection | 10 variants x 2 sector sizes; resealed credential/target/phase/baseline-length mismatch, pending/issued-generation mismatch, staging/canonical hash changes and missing staging | 20/20, zero writes/flushes, input unchanged, constructor owner released |
| Linux audits | 510 primary + 360 directed redo + 2608 pending-publication images | 3478 PASS |

All eleven ACTIVE origin roles 0..10 were observed. Primary recovery used 1014
fresh processes (762 successes, 252 intentionally interrupted); directed redo
used 546 (366 successes, 180 intentionally interrupted); power-cut recovery used
2608 successful fresh processes. Thus 4168 recovery calls include 432 deliberate
recovery cancellations. There were 1742 zero-commit second/idempotent recoveries.
No native origin sidecar supplied provenance. Actual service checks reject
unprivileged BEGIN, competing writer admission, stale authentication generations
and ordinary account-file opens while a writer owns the operation. After cuts,
process-exit retirement performs no I/O and does not re-enable authentication.
Constructor orphan cleanup precedes arming the service redo counter and is not
included in its measured semantic-commit count.

Independent Python/debugfs checks validate all 510 retained primary IDLE pairs:
matching state/origin seals, pair hash, sequence and operation generation 2;
credential generation 2; cleared target/intent/origin payload; and exact canonical
bytes, baseline lengths and hashes. They comprise 108 complete old triples and
402 complete new triples. The runner's additional independent pair assertions
were added after the OS run; retained audit.py rechecked all 510 existing dumps
without executing the OS again. Supplemental power/redo cuts use Linux fsck and
the probe's exact old/new-family checks, not the complete independent dump audit.

Accepted runtime artifacts (each raw manifest is PASS):

- `.codex-remote-attachments/pkg-account-service-durable-20261023-104155-938986/`: manifest SHA-256 `901b46b50433f650f95fd64d8b021807eabb2259065b632444cb3281d5541908`; probe `cad634f5121f84ce6afb2f267d38ecb78ac04fca1e8ab67be611faf71b0b8601`.
- `.codex-remote-attachments/pkg-account-service-power-20261023-124218-946584/`: manifest SHA-256 `5871db7d2c20b8f4c08fb3c3df50146f4bbe35cbfd1a07f25c250c6a3d9380e6`; probe `0a7cc6ffcdc7ba820394c197639b2f6ee8e31b662ef89b1b93fad3ff60e380f1`.
- `.codex-remote-attachments/pkg-account-service-redo-20261023-125034-865464/`: manifest SHA-256 `eb216ffbb78d487afc22fbfe48bea552d00ef1798dabd67fd0f270625a8a819d`.

Aggregate proof: `.codex-remote-attachments/pkg-account-service-summary-20261023/`
contains audit.py/audit.json, finalize.py and manifest.json. Runtime is PASS;
**aggregate status is FAIL_PRESERVATION_RUNTIME_PASS**, retained unchanged as a
preservation failure. The primary matrix's own preservation gate passed for all
94 compiled dependencies, all copied inputs and its contemporaneous shared-media
snapshot. AGENTS.md, README.md and the already-dirty SSL asset remain identical
at final capture. Later shared bin/fortress.elf, bin/fortress.img,
bin/fortress.iso and bin/initramfs.tar hashes differ from that snapshot. Before
and after hashes are retained in the aggregate. This task invoked no shared or
release-media build and did not restore those concurrently changed artifacts.
Uncompiled concurrent wireless changes and this task's authorized runner auditor
cleanup are listed separately in the primary manifest; they did not alter the
compiled probe inputs. Boot acceptance is NOT_RUN, independent of preservation.

Retained unsuccessful attempts:

- `pkg-account-service-durable-20261023-103203-005291`: ambiguous overlay counter
  replacement. No executable; reconstructed tool-exception note is labelled as
  such, not presented as an original runtime log.
- `...-103241-014645`: incorrect replacement anchor; same no-executable scope.
- `...-103316-293533`: strict compile failure from early POSIX macro ordering and
  copied hook indentation; corrected and rerun.
- `...-103641-866895`: 1024/512 UPDATE cut 13 plus recovery cut 1 exposed a valid
  absent-canonical/absent-TEMP state. Preview incorrectly demanded a linked TEMP;
  it now accepts absence only with the complete bound coherent stage triple.
- `...-103932-461038`: UPDATE cut 42 plus recovery cut 1 exposed success/auth
  enablement after the final IDLE cancellation. The adapter now returns EINTR;
  the original missing-interrupted-output failure, logs and images remain.

Limits and next work: this remains a private root-only account fixture. Other
PKG grant/trust/state resources and profile-policy hashes are synthetic. No
migration from generation-record v1, complete production pending/state/trust
admission, low-space/credit/pool exhaustion, charged-orphan refunds or uncertain/
torn-I/O retry acceptance is established. Power cuts cover pending publication,
not every service transaction; directed later-recovery enumeration is 4 KiB
only. Semantic cancellation is a host boundary injection, not real kernel
signals/scheduler teardown; VMM checks are host stubs. There is no new Ring 3,
QEMU, boot, release-media, installation or physical acceptance, no production
trust provisioning, no commit/push and no physical device write.


Later closeout observation, after the aggregate snapshot above: concurrent HEAD
advanced to `426a8be554b6fd4f486d7e6d557854b6af416b63`. The SSL checksum asset then
changed from `b7ed287241d30835dc5dd516c899948697e1d2eaa1f4c9a8784728ea5691e5b6`
to `f3ae0338a86c073a0988e566b7021cf2f70e2555194c5907f53aebb28d88b25d`.
The strict final assertion failed and final-checks.json retains
`FAIL_CONCURRENT_GUARD_CHANGE`; AGENTS.md and README.md still match their initial
hashes. The earlier aggregate snapshot and all raw runtime manifests remain
unchanged. This task did not restore the asset or invoke git commit/push; PKG
changes remain dirty/untracked. This is an additional preservation qualification,
not a runtime or boot result.


### Private pending/state/trust admission with native durable grants (2026-10-23)

Implemented the [bounded mount profile](../plans/PKG2_MOUNT_PENDING_TRUST.md) in
`tests/pkg_mount_candidate.{h,c}`, `tests/pkg_mount_admission.inc` and exact
copied-source overlays. The constructor validates actual marker/grant/ownership,
state, trust, immutable intent and object identities in its read-only JBD2 view
before replay writes or account recovery. Existing signed-catalog and complete
FPK/manifest/ELF verification establish archive, command, build, platform/base and
executable identity. Expected catalog keys/root/revision/hash/time remain external
policy, not disk-derived authority. The descriptor pins real fixed inode/maps.

Account generation-record v3 binds full grant/trust/state/intent hashes, grant
sequence and transaction UUID. Account mutations preserve that exact durable
snapshot. A typed sequence transaction journals ledger, ownership sequence and
both account records together, without allocating or changing G/Q. Ownership Q
is checked against actual inode generations, modes, links, dense allocations and
Linux Blockcount; effective free resources exclude G. Ordinary writes to package
record data and protected inode slots refuse before commit. General privileged
namespace dispatch, grant issuance/refunds and production integration remain
outside this private candidate.

The install-only profile accepts empty state generation 1 and one installed demo
generation 2. Both active and consumed-source predecessor hashes must match the
canonical initial state for the pinned repository/trust snapshot. PENDING admits
old state with absent/new command, or exact new state/new command. RETIRING keeps
the immutable capability under retired.v1; it does not erase authorization before
cleanup. All required archive/activation/next-state/intent roles and actual
ownership identities remain checked. Unknown operations/step bits, paths, flags,
unsupported ownership classes, missing evidence or contradictory state refuse.
Existing recovery may finish the exact admitted intent after catalog expiry;
a new RESERVED admission must meet current validity. No root rotation is added.

Final runtime matrix under ASan/UBSan and strict compiler warnings:

| Filesystem block | Sector sizes | Phase/publication states | Selected account cuts | Grant callback count (512/4096) | Directed grant I/O cuts | Zero-I/O rejects | Linux audits |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1024 | 512,4096 | 14 | 56 | 63 / 34 | 48 | 238 | 236 |
| 2048 | 512,4096 | 14 | 56 | 89 / 26 | 48 | 238 | 236 |
| 4096 | 512,4096 | 14 | 56 | 141 / 22 | 48 | 238 | 236 |

All 42 phase/publication cases PASS. UPDATE commit counts are measured per
layout: all fourteen 1 KiB layouts use 42; 2 KiB uses 42 for its four IDLE
layouts and 41 for the ten active layouts; 4 KiB uses 42 for its two empty-IDLE
layouts and 41 for the remaining twelve. Cuts are 1, 13, 29 and the measured last
commit, not an assumed universal 42. Every cut recovers in two fresh processes.
The 144 grant I/O cases select first/middle/last callbacks under cached/all/
alternating/inverse persistence, before and after each selected callback. This
is a directed sample, not an exhaustive callback sweep.

The 714 read-only refusals include independent policy/root/floor/hash/key/base/
platform/clock mismatches; resealed account/grant snapshots and false ownership
allocation/generation/orphan claims; wrong operations/steps; pending generation,
transaction, trust, state and time; invalid publication history and IDLE command/
archive identities. Six fully hash-pinned forged catalogs still fail signature
verification. Six valid signed two-file artifacts reject this one-file profile
and clear their verification bit. Six consumed-source foreign-genesis fixtures
reject before replay. Each refusal requires zero underlying writes/flushes,
unchanged image bytes, no published mount and released constructor ownership.
There are also 630 ordinary record-write refusals without I/O or mount taint.

There are 42 committed-uncheckpointed previews whose grant sequence is newer
than home, followed by actual replay/recovery. An additional 42 fresh processes
reopen their recovered images: zero semantic commits and byte-identical grant,
ownership, state, trust, account-generation and origin record sets. Across the
matrix and supplement: 786 successful fresh `recover` process calls, 396
idempotent second recoveries, and 750 Linux audits (708 matrix + 42 supplement).
The Linux oracle checks record seals and cross-hashes, sequence, T=G+Q, exact
state/trust fixture bytes, actual inode generation/owner/mode/links/size and
allocated-sector counts. No origin sidecar supplies provenance.

The pure validator's original 1056-byte frame failed the strict ceiling. Policy
and canonical-initial-state scratch now belong to the caller workspace. Current
freestanding stack usage is 816 bytes, dynamic/bounded; <=1024 frame gate passes
for all three builds. This does not establish a full kernel call-chain bound.

Final runtime artifacts (all raw manifests retain FAIL for shared-media guards;
all runtime cases and private/compiled-input guards PASS):

- `.codex-remote-attachments/pkg-mount-admission-20261023-123852-363077/`: manifest `a015648ddf873281abb5dbbb6f7547170cdac6e3adc5d5220619af1171456171`; probe `950be31f77d65f87d5fb2a5d46566ce7d4ffbcd1188335188cac525b5d00707b`.
- `.codex-remote-attachments/pkg-mount-admission-20261023-123852-363184/`: manifest `998fe9d15a3cd73b87be977c6f73be103ec654160dfbe73ee11eccd3e93e1159`; probe `d48b45bca6e4ccbb5832376ca6c3c96cdd6833215979c57221028eb00d367826`.
- `.codex-remote-attachments/pkg-mount-admission-20261023-123852-363078/`: manifest `467a02fc0591d0f1c4fe46e5e1263ccf91cddbcd6b6fa4e9ae469b1e9db5cd90`; probe `3f0cc9332c5e9356ddbe2e513c087148b5d0bafb7fae7d65ec7e34cf96834f69`.
- `.codex-remote-attachments/pkg-mount-repeat-20261023-144735-942793/`: manifest `5799f15b088e7e350efef1e81b48a552c22856ba8e24effa83007e84c9f381f6`, 42/42 repeat recoveries PASS.

Aggregate: `.codex-remote-attachments/pkg-mount-summary-20261023/manifest.json`,
with finalize.py, attempts.json and append proof. Status is deliberately
**FAIL_PRESERVATION_RUNTIME_PASS**. Each final build preserves all 112 compiled
input paths, all copied inputs and 102 normalized repository dependencies.
AGENTS.md, README.md and the already-dirty SSL asset match their initial hashes
at aggregate capture. Shared bin/fortress.elf, fortress.img, fortress.iso and
initramfs.tar changed concurrently; before/after hashes and original FAIL results
remain. This task built only private host probes and did not restore shared
artifacts. Concurrent uncompiled iwlwifi.c changes are retained separately. HEAD
advancement is observed, not a commit by this task. No new boot/media acceptance.

Retained findings and unsuccessful/superseded attempts:

- `pkg-mount-admission-20261023-115629-049621`: strict indentation compile failure;
  `...-115815-784982`: missing host-only tool_syscall trap at link; both corrected.
  An earlier Python overlay indentation/import failure occurred before any
  evidence directory; the summary labels its reconstructed exception note.
- `...-120147-041283`: fourteen smoke cases passed, shared-media preservation
  failed. `...-120605-627224`: fourteen cases and initial negatives passed, then
  compiled repository source changed during review; the guard FAIL is retained.
- The three `...-121254-*` attempts failed compiling an unavailable strchr helper.
  It was replaced by the existing bounded-scan pattern. No runtime acceptance.
- `...-121555-149514` and `...-121555-149533`: hard-coded UPDATE cut 42 failed
  because those layouts have 41 actual commits. Measurement now selects the
  layout's real last commit. The 1 KiB `...-121555-149646` and `...-121913-*`
  checkpoints remain historical, not final-source acceptance.
- `pkg-mount-unsupported-20261023-142602-831681`: valid signed unsupported archive
  reproduced stale verified=1 on profile rejection. Fixed by clearing success
  on every rejection after artifact verification, with result output preserved.
- `pkg-mount-unsupported-20261023-143317-715116`: actual unexpected acceptance of
  a consumed-source foreign predecessor. Fixed by canonical generation-1 lineage
  and generation-2 installed-state checks. Its initial generic manifest scope
  text was inaccurate; a separate scope-correction.json records the one-file
  foreign-genesis case without rewriting the original manifest.
- The three `...-122637-*` runtime PASS checkpoints predate the lineage fix and
  are superseded. The three `...-123532-*` host campaigns were deliberately
  interrupted for final review hardening; their INCOMPLETE results remain.
  These host SIGINTs are not kernel signal/teardown acceptance.
- Final `...-123852-*` runs preserve their raw shared-media FAILs; no rerun was
  used to erase them. attempts.json indexes all 21 matrix attempts and hashes.
  The stack failure and corrected output are retained separately under
  `.codex-remote-attachments/pkg-mount-frame-20261023/`.

Scope limits: one application/install, fixed trust policy, root-only host account
fixture and initialized native grants. No general inventory/migration, root
rotation, production boot selector, production trust provisioning, runtime grant
issuance, charged-orphan refund/release, minimum-space progress, torn-sector or
uncertain-I/O retry acceptance. The conservative account floor can refuse valid
near-full roots; low-space liveness remains open. Existing Ring 3/QEMU/physical
claims are unchanged; this task adds none. Production integration and installation
remain disabled. All PKG changes are uncommitted; no commit/push, release-media
build or physical-device write was performed by this task. Next work is low-space
recovery and correctly charged orphan cleanup, then actual kernel teardown tests.


### Private low-space recovery and charged-orphan refunds (2026-10-23)

[Contracts, exact evidence and retained failures](../plans/PKG2_LOW_SPACE_REFUNDS.md). The private mount/account BEGIN gate now uses actual remaining fixed-role capacity, including account orphan allocations, outside durable package escrow. Only fixed-role account allocation may consume that capacity; competing ordinary block/inode allocations reject before I/O. An isolated old-code reproduction retained the reserve-stealing defect (ordinary creation succeeded with 125 private-model I/O events), which the new probes detect.

Final effective-space and physical-exhaustion matrices each pass 42 states across 1/2/4 KiB blocks × 512/4096-byte sectors. Together: 624 interruption cases (two fresh-process recoveries each), 1,548 pre-replay zero-I/O refusals, 720 ordinary competing-allocation zero-I/O refusals and 1,458 Linux audits. Actual initial F-G equals remaining account capacity of 80/35/17 blocks and eight inodes. Account-orphan frees restore R; they do not refund package G.

The charged lifecycle workbench now derives refunds from equal observed free-counter increases and owned-allocation decreases, preserving T=G+Q in actual JBD2 commits. It passes 10,152 exhaustive 4 KiB I/O cuts and 576 selected cuts across all six geometries, each followed by two actual constructor restarts; 16 retained-output Linux audits pass. The charged target is one inline one-block inode; multi-file/fragmented/deep-tree cleanup is not established. Arithmetic checks accept 28 valid reclaim observations and reject 2,132 mismatches; repeated observations grant no extra credit.

Shared media, AGENTS.md, README.md, SSL checksum asset and compiled inputs are preserved in the final campaigns. Earlier compile, wiring, BEGIN-headroom and preservation failures, old-context reproductions and superseded PASS checkpoints remain recorded. No new Ring 3, boot, SMP, scheduler or physical acceptance is claimed; no release-media build, installation, production trust provisioning, push or physical-device write was performed. The user subsequently authorized a local PKG checkpoint commit.

Still open: authenticated retirement capability and atomic package-refund/account-generation integration. The signed pending/trust mount profile still refuses package-charged orphans and retains archive/state/intent evidence; the refund lifecycle is a separate private grant profile. Production integration and installation remain disabled.
