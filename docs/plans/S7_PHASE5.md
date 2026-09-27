# S7 Phase 5: stream utilities implementation plan

**Status: Phase 5A COMPLETE; user accepted successful tests on 2026-09-27. Phase 5B remains open.** Prepared 2026-09-27 against completed
Phase 4 (`03da998`, acceptance recorded in `aacfbbf`). See the
[acceptance record](../roadmap/shell-s7-phase5a.md) and [next implementation plan](S7_PHASE5B.md).
User review incorporated: zero-count tail drains, ASCII word semantics, explicit
overflow/status rules and a file-by-file cat/view migration checklist.

## Scope and decisions for review

Deliver freestanding `/bin/cat`, `/bin/head`, `/bin/tail` and `/bin/wc`, usable
directly and through the existing external pipeline executor. Preserve BSP
affinity, flat parser output, eight-stage limit, lexical redirections, last-stage
status and cooperative cleanup. No new syscalls, seeking, allocator, signals,
cancellation, scheduler changes or cross-core acceptance.

The master plan also assigns builtin pipeline-stage support to Phase 5. Split
the work into **5A: stream utilities** (this detailed plan) and **5B: builtin
pipeline stages** (a separate design/implementation checkpoint). Completing 5A
does not complete all of Phase 5. In 5A, `echo hello | wc -l` remains unsupported;
use external producers or input files. Do not relax the builtin rejection guard
or execute a builtin in the parent while consumers have not yet launched.

The implementation incorporates these reviewed refinements:

1. Remove `cat` from builtin dispatch/discovery. Preserve its existing sanitized
   text-viewing behavior under a `view` builtin, with updated help and completion.
   View preserves printable ASCII, tab and LF, substitutes `.` for other bytes,
   and adds LF to nonempty unterminated output; it is not a byte-preserving copier.
   Bare `cat` then follows normal PATH resolution to the byte-preserving tool;
   `/bin/cat` works regardless of PATH. Do not special-case only pipeline `cat`,
   which would give the same command different data semantics in and out of pipes.
2. Replace §2.9's silent `tail` line truncation with explicit failure when a
   retained line cannot be represented. Keep the bounded-memory approach below.
   This supersedes the older silent-truncation detail in the master plan.

## Command contract

All commands accept zero or more file operands: none means fd 0; `-` means the
current stdin stream at that position. Process files sequentially, one opened
input at a time. Repeated `-` does not rewind stdin. Open operands read-only and
close only descriptors owned by the utility, even when an open returns 0, 1 or 2.
Do not close inherited fd 0 merely because it supplied input. Treat directories
as errors; do not require a nonzero stat size or seekability to consume a stream.

Parse and validate all options before opening or reading input. Support `--`,
with options before operands; after the first operand, remaining words are
operands. Names beginning with `-` require `--` unless exactly `-`. Unknown
options, missing counts and invalid counts are usage errors. `head`/`tail` accept
`-n N` or `-c N`, exactly one count option, default `-n 10`. Reject signs, suffixes,
attached counts (`-n10`), empty counts, mixed/repeated modes and decimal overflow.
These are deliberately bounded interfaces, not claims of full POSIX/GNU parity.

| Tool | Required behavior |
| --- | --- |
| `cat [--] [FILE ...]` | Copy every input byte, including NUL, high bytes and control characters. No substitution, labels, separators or added newline. No other options. |
| `head [-n N \| -c N] [--] [FILE ...]` | Emit the first N lines or bytes per operand, default 10 lines. LF ends a line; preserve an unterminated final fragment. Stop immediately when satisfied; do not drain the remaining pipe. No multi-file headers. |
| `tail [-n N \| -c N] [--] [FILE ...]` | Consume each operand to EOF, then emit its last N lines or bytes in original order. Preserve termination exactly; no multi-file headers. Bounded storage and limits below. No `-f` or `+N`. |
| `wc [-l] [-w] [-c] [--] [FILE ...]` | Default all three counts; selected fields always ordered lines, words, bytes. Accept separate flags or bundles such as `-lw`; repeated flags are idempotent. Count lines as LF bytes, bytes as all bytes, words as maximal runs outside ASCII space/tab/LF/CR/VT/FF. No locale or Unicode decoding. |

The wc word definition is deliberately byte-oriented and ASCII-only: separators
are space (0x20), tab (0x09), LF (0x0A), CR (0x0D), VT (0x0B) and FF (0x0C).
Unicode-aware word counting would require an encoding layer, outside this scope.

`head -n 0` and `head -c 0` produce nothing and do not read input; still open and
close named operands to report path errors. Named operands are opened even with
zero counts so status reflects the operands requested: `head -n 0 missing.txt`
reports `head: cannot open missing.txt` on stderr and exits 1.

**`tail -n 0` and `tail -c 0` drain each operand through EOF and emit nothing.**
They use the shared read buffer without retaining lines/bytes, still propagate
open/read errors, and close owned inputs. Thus a finite successful producer in
`producer | tail -n 0` can finish normally rather than receive EPIPE merely
because the requested count is zero. All tail counts consume through EOF; a
never-ending input does not complete. Head's zero-count early exit is different.
Tail's `--help` explicitly says: "Tail always reads to EOF, including count zero;
it cannot stop a producer early." This describes normal consumption, not recovery
from an input failure.
Counters use checked `uint64_t`; head counts may reach UINT64_MAX, while tail
counts must also pass its storage limits.

`wc` prints decimal fields separated by one ASCII space, with one LF. For implicit
stdin, omit the name. For explicit operands, append a space and the operand name,
including `-`. With multiple operands, append a `total` row for successfully read
operands. A read-failed operand gets a diagnostic and no count row; it contributes
nothing to totals. Any failure still makes the final status nonzero. Check both
per-file counts and total additions for overflow; never print wrapped values.
Counter overflow reports `wc: counter overflow` on stderr and sets exit status 1.
A per-file overflow suppresses that operand's entire row and contribution to
totals; continue with later operands. On total-addition overflow, suppress the
total row, retain valid individual rows and keep status 1. Check each row's
counters before writing any of its fields; previously emitted rows are not undone.

## Shared runtime and errors

Add `user/tools/common.h`, `common.c` and `start.asm`. Use the existing public
`syscall_abi.h`, `types.h` and VFS flags. No hosted headers or dependency on shell
UI, history, environment storage or editor code. Extract/reuse the semantics of
`write_bytes_fd`, not its whole shell module. Suggested internal entry point:
`int tool_main(int argc, char **argv)`; the aligned entry stub passes the existing
loader arguments, calls it, then issues SYS_EXIT with its returned status.

Use a 4096-byte static I/O buffer and BSS tail storage. Every read handles a
positive short result, zero EOF and negative error. `write_all` advances over
positive short writes, stops on negative errors, and treats zero progress as EIO.
No retry loop on EPIPE and no busy polling. Diagnostics use fd 2 directly and
must not recursively diagnose a failed diagnostic write.

| Outcome | Exit behavior |
| --- | --- |
| All requested operations succeed | 0 |
| Open/read/close error, zero-progress write, overflow or retained-tail capacity error | Diagnostic on stderr; 1 |
| Invalid options or out-of-range requested count | Usage diagnostic on stderr; 2, before input I/O |
| stdout returns `SYSCALL_EPIPE` (already negative) | Stop immediately, close owned input, exit 141; no broken-pipe noise |

Input errors may continue to later operands, with final status 1. Output errors
stop the whole command; continuing cannot produce a trustworthy result. Preserve
an earlier output error if cleanup also fails. Diagnostic failure must not replace
the original status. Head's successful early exit stays 0; an upstream tool's 141
does not change the pipeline's last-stage status. For standalone external commands,
update `spawn_program()` in `user/shell.c`: its current fault-reporting branch
was verified before editing as `status >= 128 && status < 160`. The kernel's
`src/arch/x86_64/idt.c` terminates faulting user processes with `128 + frame->vector`;
the 32 exception vectors explain the exclusive upper bound 160. Use
`status >= 128 && status < 160 && status != 141` so the existing
nonzero-exit branch prints `[PROCESS] Exit status 141`, preserving the returned
status. Keep other reporting behavior unchanged. Without termination metadata,
128+N can represent an explicit exit code or a fault/signal convention; the number
alone cannot distinguish them. Do not claim that the current code tests 128..255
or introduce signal delivery as part of this change.

## Tail storage and algorithm

There is no seek syscall in the current ABI. Use the same streaming algorithms
for pipes and files; do not add hidden whole-file allocation or temporary files.

- Byte mode: 65,536-byte BSS ring, count range 0..65,536. Track write index and
  valid-byte count. At EOF, write up to two spans from the oldest retained byte.
  Work and storage must not grow with input length.
- Byte requests above 65,536 are usage errors (status 2), rejected before opening
  any operand or performing input I/O. Zero bytes uses the drain-only path.
- Line mode: a 40 KiB **content-storage** budget divided by a 4096-byte maximum
  content length per slot yields `floor(40960 / 4096) = 10` retained lines.
  This is a fixed-slot design: many short lines do not increase the slot count.
  Ten slots each store up to 4096 content bytes plus an optional LF, along with
  length/termination/overflow metadata. Accept `-n 0..10`; larger requests fail
  with status 2 before opening/reading, with an explicit maximum-count diagnostic.
  LF bytes and metadata are additional to the 40 KiB content budget, as accounted
  below; a strict 40 KiB total including them would not fit ten such slots.
- Rotate slots only when a following line actually starts, not immediately on
  LF. A terminal LF must not create a spurious empty final line. An unterminated
  nonempty fragment counts as a retained line; empty input has no lines.
- When a line exceeds 4096 content bytes, retain an overflow flag and discard
  further content through its LF. Continue scanning so later lines can evict it.
  At EOF, if any selected slot overflowed, report `tail: retained line exceeds
  4096 bytes`, emit no output for that operand, and return failure. An overlong
  line outside the selected suffix must not cause failure.
- Before writing any selected lines, check all selected overflow flags. Output
  selected slots oldest first, preserving every stored byte and original LF.

The BSS budget is roughly 40 KiB plus 10 bytes/metadata for line mode and 64 KiB
for byte mode, plus the shared I/O buffer. A union may share these mutually
exclusive stores. No slot array, ring or 4 KiB buffer goes on the user stack.
This count/line-length limitation must appear in help and tests; increasing it
requires a deliberate revised memory contract, not silent truncation.

## Implementation sequence and files

1. **Runtime/build scaffold.** Add shared helpers and entry stub. Add explicit
   Makefile ELF/header/linker dependencies using existing freestanding flags,
   `-fno-pie`, no red zone/SIMD, static linking and page-separated permissions.
   Review `.su` reports and the nested call paths against the 4 KiB stack and
   existing 512-byte minimum floor; no large automatic buffers or recursion.
2. **Cat and shell migration.** Implement `user/tools/cat.c`; package `/bin/cat`.
   Rename the sanitized builtin to `view` in `user/shell.c`, builtins header/table,
   help and applicable tests. Confirm `builtin_find("cat") == CMD_UNKNOWN` so
   pipeline preflight naturally accepts bare cat. Check `type`, `command cat`,
   PATH overrides, relative paths, stdin and redirection-only regressions.
3. **Head and wc.** Add independent sources with common option/count handling.
   Head tracks remaining bytes or LF count across reads, emitting only the
   selected prefix of the final chunk. Wc keeps word-state across read boundaries,
   resets it per operand, and checks additions before formatting bounded decimals.
4. **Tail.** Implement byte ring first, then line slots and overflow metadata.
   Reset all state per operand; verify wraparound and the trailing-LF rules.
5. **Packaging and acceptance.** Make all four ELFs production initramfs
   dependencies, staged at `/bin/{cat,head,tail,wc}` in USTAR. Update initramfs
   help text and documentation. Keep `/bin/pipetest` confined to disposable test
   ISOs. Add the host suite and extend the existing S7 integration runner.

Suggested test files: `tests/stream_tools_host.c` and
`scripts/test_stream_tools_host.py`. Link actual utility sources under distinct
entry names with mocked syscall adapters; do not duplicate algorithms in tests.
Add `make test-stream-tools-host`, included in `test-host`. Extend
`scripts/test_shell_s7.py` and its existing `make test-shell-s7` target rather than
forking another UART prompt parser. Keep the Phase 4 redraw regression coverage.

### Cat/view migration checklist

Audit references with `rg -n '\bcat\b|CMD_CAT|builtin_name|builtin_count' user tests scripts Makefile`.
Do not mechanically rename every cat invocation: raw file/stream tests must keep
testing the new external cat; only sanitized-viewing/builtin expectations migrate.

| File | Required review/change |
| --- | --- |
| `user/shell.c` | Rename sanitized cat helper and dispatch to view; remove cat builtin dispatch; adjust the standalone 141 reporting branch. |
| `user/shell/builtins.h` | Replace `CMD_CAT` with `CMD_VIEW`; audit enum consumers. |
| `user/shell/builtins.c` | Replace builtin name/help entry; describe external cat and sanitized view accurately. |
| `user/shell/complete.c` | Verify both builtin enumeration paths use the renamed table and filesystem completion finds `/bin/cat`; add no hardcoded cat entry. |
| `user/shell/pipeline.c` | Keep generic builtin rejection; verify external cat is accepted and view remains rejected. |
| `tests/shell_host.c` | Retain `cat < input.txt` parser coverage; add cat/view classification and sanitized-view regression coverage where appropriate. |
| `tests/pipeline_host.c` | Add bare cat acceptance and view rejection with no preflight side effects. |
| `scripts/test_shell.py` | Retain file-output checks against external cat; update missing-file/directory diagnostic assertions to the utility contract rather than the former builtin's strings. |
| `scripts/test_shell_s3_s4.py` | Keep relative-path cat test; add type/PATH/view discovery coverage as needed. |
| `scripts/test_shell_s6.py` | Keep raw file/stdin/redirection checks on cat; move only sanitization or parent-builtin-specific expectations to view. |
| `scripts/test_shell_s7.py` | Keep canary reads on external cat; add utility pipeline and view-rejection cases. |
| `scripts/test_shell_prompt_host.py` | Keep textual cat capture examples; they test prompt framing, not builtin classification. |
| `Makefile` | Package `/bin/cat` and revise generated initramfs readme to distinguish it from view. |

Trace completion's builtin-name enumeration and filesystem candidates so view
appears once as a builtin and cat is discovered from `/bin`. Add completion
coverage at the actual implementation/test sites found by the search; avoid
inventing a separate hardcoded cat completion entry.

## Acceptance gates

| Area | Required evidence |
| --- | --- |
| Runtime and CLI | Inject short reads/writes, EOF, zero progress, EIO, EBADF, EPIPE, failed open/close and closed stderr. Validate `--`, missing/malformed/overflow counts, flag bundles, file names beginning with `-`, repeated stdin and sequential cleanup. |
| Cat | Empty input, all 256 byte values, no final LF, multiple files with stdin between them, binary equality through 2/3/8 stages. No forced output newline. |
| Head | Counts 0, 1, default 10 and UINT64_MAX; zero-count missing-file diagnostic/status 1 with no reads; LF on either side of read boundaries, long lines, fewer lines than requested, unterminated final fragment; no further read once satisfied. |
| Tail | Byte counts 0, 1, 65,536 and rejected 65,537; line counts 0, 1, 10 and rejected 11; zero-count reads through EOF with no output, including read failure; invalid counts cause no opens/reads. Empty input, terminal LF, consecutive empty lines, long fragment, exact 4096-byte content, retained overflow versus evicted overflow, multiple ring wraps. |
| Wc | Empty input, LF versus final fragment, all six ASCII separators, NUL/high-byte word data, words spanning reads, option order, exact row formatting, multiple-file totals, failed operands; inject per-file and total overflow and assert diagnostic/status 1, suppressed affected rows and no wrapped/partial counts. |
| Shell integration | Bare `cat` and `/bin/cat` agree; `view` preserves sanitized viewing; stateful builtins remain rejected in pipelines. Test `cat FILE \| head -n 50 \| wc -l`, `cat FILE \| tail -n 10 \| wc -l`, redirection precedence, last-stage status, negation and prompt recovery. |
| EOF/EPIPE | Large cooperative producer into `head -c 1` and zero-count head: upstream EPIPE yields 141 without fault-labelled output. Into zero-count tail: producer drains successfully and exits 0. Reap every PID; pipeline remains last-stage status. A dedicated Ring 3 harness waits on the producer directly to observe its status; pipeline `$?` alone cannot prove either outcome. |
| Lifecycle | Repeated pipelines plus existing pipe/resource suites; mock exact descriptor balance and real child/reaping observations. Prompt recovery alone is not exact kernel allocator/refcount evidence. |

Host tests run ASan/UBSan and compare explicit expected byte arrays. Standard
host tools may be additional reference oracles only on the supported overlapping
semantics; do not assume identical formatting/options for the bounded interfaces.

QEMU acceptance uses BIOS and UEFI, one CPU, disposable ISO/NVMe copies and paired
read-only OVMF code/disposable vars, bounded host waits, unconditional QEMU cleanup
and retained logs. Compare binary output offline, not through terminal rendering.
Use a 262,267-byte disk payload to exceed 256 KiB within the current 1 KiB ext2
single-indirect write limit; larger streaming-only fixtures can be verified by a
consumer without writing oversized files. Extract output and compare every byte,
then run offline `e2fsck -fn` on the disposable filesystem.

Recommended user-run sequence after implementation:

```sh
make
make test-stream-tools-host
make test-shell-s7
make test-host
make test-pipe
make test-shell-s6
make test-shell-s6-resources
```

Record commands, firmware, fixtures and results when actually run. The user owns
test execution unless they request otherwise. Mark 5A complete only after its
acceptance; keep 5B open until builtin-stage support is designed and accepted.
Cross-core and physical-hardware claims remain Phase 6 gates. `tee`, `printf`
and `grep` remain S7.5 scope.

Implementation/build handoff: [Phase 5A roadmap](../roadmap/shell-s7-phase5a.md).
