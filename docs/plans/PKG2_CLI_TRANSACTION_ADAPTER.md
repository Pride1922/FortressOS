# Private CLI transaction adapter

The trusted embedding entry `pkg_transaction_cli` adds `pkg status` and
`pkg recover` through an explicitly supplied backend. Existing inspect/verify
commands delegate to the original CLI. This is a process-local C interface,
not a syscall ABI. It accepts no device path, trust key, force flag, install,
remove or update operation.

The default pkg_main, existing cli.c/cli.h, default build and shared media remain
unchanged. The new optional frontend source is not linked into the default
/bin/pkg. There is no production backend, trust provisioner or installation
path. The private host adapter uses the actual accepted EXT4/JBD2 and account
code with the same independently pinned fixture policies and certificates.

Status runs the genuine read-only journal preview and native admission checks,
captures the authenticated package record snapshot, then terminates construction
before replay, dirty marking or namespace publication. It can therefore show a
committed-uncheckpointed IDLE state without writing. On an already owned private
mount it validates and copies the snapshot under filesystem exclusion. Output
occurs after exclusion is released.

Recover admits the fixture through the real constructor, completes the accepted
retirement-to-IDLE path, prepares the actual account service and validates the
terminal snapshot. A backend failure is reported as an unconfirmed outcome;
there is no retry or rollback in the CLI. A successful mutation followed by
stdout failure returns failure without undoing or repeating the durable action.
The next status invocation can still observe IDLE.

Both commands require exact arity. Malformed arguments, absent callbacks and
unsupported operations never reach storage. Backend errors, invalid phases,
missing generations, overflowing resource sums and a non-IDLE recovery result
cannot print success. Backend/context/policy lifetime and serialized calls are
trusted-embedding requirements, not user-controlled command-line parameters.

## Acceptance (2026-10-23)

**PASS.** The private CLI-to-engine connection is complete for status and recovery.
Changes remain uncommitted. The accepted underlying engine checkpoint is 9e136f3;
this continuation changes none of its existing implementation files.

Strict GCC warnings and ASan/UBSan pass. Six block/sector geometries each execute
11 scenario groups and 24 selected before/after interruption cases (first, middle,
last write/flush event across four persistence modes). Each interruption is
recovered through the actual CLI entry in two fresh processes. All resulting five
record sets exactly match the independently signed IDLE vector, including grant,
account and credential/operation generations; repeat recovery makes zero
retirement commits. These CLI cuts are selected coverage, not a new exhaustive
engine campaign. The engine's preceding exhaustive acceptance remains separate.

| Block/sector bytes | Recover I/O events | Injected interruptions | Fresh recoveries | Linux audits |
| --- | ---: | ---: | ---: | ---: |
| 1024/512 | 908 | 24 | 48 | 4 |
| 1024/4096 | 485 | 24 | 48 | 4 |
| 2048/512 | 1402 | 24 | 48 | 4 |
| 2048/4096 | 397 | 24 | 48 | 4 |
| 4096/512 | 2390 | 24 | 48 | 4 |
| 4096/4096 | 353 | 24 | 48 | 4 |

Totals: **66 CLI scenario groups, 144 interruptions, 288 fresh recoveries and
24 Linux fsck/record audits**. Audits apply to the retained normal recovery,
repeat recovery, stdout-failure-after-commit and committed-journal replay outputs,
not every temporary cut image. Passing cut images are deleted only after both
fresh-process checks; failed images/logs are retained.

The scenarios include:

- RETIRING and IDLE status; committed-uncheckpointed IDLE status with zero
  storage writes/flushes and no mount publication.
- Actual recovery to IDLE, repeat recovery without extra credit, and journal
  replay after a real terminal commit pause.
- Stdout failure after eleven durable retirement commits: CLI returns failure,
  calls recover once, and subsequent status still reports IDLE without I/O.
- Invalid arity, raw device/force arguments, install, long/null arguments,
  missing backend, and the original default pkg_main refusing status/recover
  before storage. Private help is also covered without storage access.
- Five invalid/failing status responses, missing status/recover callbacks and
  a successful backend response that remains RETIRING; none prints success.
- Damaged on-media completion evidence refusing both status and recovery with
  zero storage writes/flushes; no false IDLE or recovery-complete output.

The pre-existing actual inspect/verify host suite also passes through the new
frontend with a null backend: read-only opens, descriptor cleanup, supplied-policy
verification, runtime clock, artifact/catalog corruption and read/stat/close/output
failures. Its syscall transport is mocked; actual verifier/crypto code runs.

The new frontend compiles freestanding with no red zone/SIMD/host runtime and a
1024-byte frame ceiling. Measured optimized frames are 96 bytes (number output)
and 128 bytes (entry); this is not a transitive crypto or kernel stack claim.
No new ELF, Ring 3, boot, SMP or physical acceptance is inferred.

Evidence:

- .codex-remote-attachments/pkg-cli-transaction-20261023-222029-442803/:
  retained compile FAIL from the duplicate tool_syscall trap. The old private
  verifier trap is removed only from copied sources; the new stdout/stderr
  transport still asserts on unexpected filesystem syscalls.
- .codex-remote-attachments/pkg-cli-transaction-20261023-222344-700970/:
  focused 4 KiB/4 KiB PASS, superseded by the full geometry campaign.
- .codex-remote-attachments/pkg-cli-transaction-20261023-222711-611653/:
  final six-geometry PASS with immutable copied/compiled inputs.
- [Exact aggregate counts, hashes and preservation](../../.codex-remote-attachments/pkg-cli-transaction-summary-20261023/manifest.json).

Independent reconciliation rechecks all compiled copies, repository inputs and
guards after completion. AGENTS.md, README.md, existing cli.c/cli.h, the default
Makefile, SSL checksum and shared ELF/initramfs/ISO/raw image remain unchanged.
Source/media preservation is separate from boot acceptance. Historical roadmap
bytes are preserved by append-only update.

Run from WSL:

    make -f tests/pkg_grant_candidate.mk test-pkg-cli-transaction

## Boundaries

 The accepted engine profile remains one installed
demo with single-block temporary objects. Host stdout/stderr transport is mocked;
the filesystem, journal, crypto and account code are actual copied sources.
This does not establish Ring 3 delivery, a kernel package API, production
privilege/session binding, general installs/upgrades/removals or physical
power-loss recovery. Production integration, installation, trust provisioning,
release-media builds, pushes and physical-device writes remain disabled.
