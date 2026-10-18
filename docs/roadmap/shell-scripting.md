# Shell scripting Phases A–D — implementation and evidence

2026-10-18. **Implemented and verified in the isolated `shell-scripting` worktree.**
Based on `036a616`. Not merged, deployed or included in a rebuilt
release image. Watchtower's concurrent changes were not modified or integrated.
The source plan is [SHELL_SCRIPTING_PLAN.md](../plans/SHELL_SCRIPTING_PLAN.md).

## Delivered subset

`/bin/sh` is packaged from the same ELF as `/bin/shell`. Explicit script files,
`-c` command text and nonterminal stdin run without UI initialization, MOTD,
automatic history activity or aliases. The shared lexer/executor handles
comments, quoting, variables, lists, pipelines and redirection. LF/CRLF,
continuations, quoted multiline text and a final unterminated line are supported.

```sh
sh /home/operator/check.sh example.com
sh -c 'echo "$0:$#:$1"' check example.com
printf 'echo from_stdin\n' | sh
sh -c 'exit 7'; echo "$?"
```

File scripts are read from a separate CLOEXEC descriptor. Child commands keep
the caller's stdin; stdin scripts read without read-ahead and share fd 0 with
their commands. Redirect command input explicitly when needed.

`$0`, `$1`..`$9`, `${10}`..`${31}`, `$#`, `${#}`, `"$@"`, `"${@}"` and
`shift [count]` use owned, bounded positional storage. Quoted argument lists
preserve spaces and empty arguments; zero parameters yield zero arguments for
a standalone quoted `$@`. Prefixes join the first argument and suffixes join
the last. An excessive shift fails without changing arguments. Existing spawn
limits still constrain supplied arguments (32 argv entries, 256 bytes per
string and 1024 total bytes, including terminators and interpreter operands).

The production shell startup stub now returns the shell's status through
SYS_EXIT. Test programs sharing the stub retain their previous behavior through
an assembler flag. Empty scripts return zero; normal command failures do not
abort later commands; syntax/input failures return 2. `exit` accepts 0..255.

Script child launches retain the interpreter's process group and inherited
streams; they do not hand off the terminal or use interactive staged group
release. SIGCHLD stays masked across registration, and durable wait records
cover children that finish immediately. Failed pipeline launches kill/reap
owned child PIDs. Normal script exit sends HUP/CONT to tracked live children,
allows a bounded grace period, then escalates to KILL and reaps them without
consuming terminal input. INT/TERM/HUP cancellation interrupts further commands.
Interactive staged group creation and terminal handoff remain unchanged.

## Verification

All commands run in WSL Ubuntu-24.04 from the worktree:

- `make build/shell.elf`: freestanding build PASS.
- `python3 scripts/test_shell_host.py`: actual script-reader and shell
  ASan/UBSan checks PASS. Covers CRLF, continuation, multiline quotes,
  incomplete EOF, syntax stopping, command-size/NUL/read failures, cancellation,
  positional cardinality and joining, atomic argument-limit rejection,
  shift bounds, escaped dollar and unsupported parameter forms.
- `python3 scripts/test_pipeline_host.py`: pipeline orchestration PASS.
- `python3 scripts/test_s8_jobs_host.py`: interactive job-control regression
  and noninteractive launch/failure-cleanup checks PASS under ASan/UBSan.
- `python3 scripts/test_shell_scripting.py --base /mnt/c/Sources/FortressOS`:
  **108/108 guest cases PASS**, BIOS/UEFI × SMP=1/4, disk-free. Each of four
  boots runs 27 cases: script/command/stdin modes, status propagation,
  parameters, nested foreground/background scripts, redirected child stdin,
  continuation/quotes, syntax/bounds rejection, pipeline execution, background
  cleanup, Ctrl+C status 130 and return to the interactive prompt.

Guest assertions compare selected output markers/statuses, not entire serial
transcripts byte-for-byte. Host tests inspect actual expanded argv values.
Neither suite claims a complete resource-counter audit or arbitrary descendant
containment; tracked child PID cleanup and launch ordering have focused coverage.

The runner copies existing media inputs into a temporary directory and injects
only the worktree shell and test scripts. It attaches no data disks. Temporary
ISOs/firmware variables are deleted on exit. SHA-256 checks confirmed the base
IMG, ISO, initramfs and fixture kernel were unchanged throughout acceptance.
The ignored `build/shell-scripting-result.json` records the matrix, shell digest
and preserved input digests. No primary release images were rebuilt.

## Remaining boundaries

This is a FortressOS subset, not a POSIX/Bash compatibility claim. Source,
functions, substitutions, traps, arithmetic, here-documents
and automatic shebang dispatch are not implemented. Known compound keywords
in unsupported grammar positions are rejected before executing that command unit.
Script diagnostics currently identify input name and starting line; precise
token columns remain a follow-up. Existing shell variable/assignment rules are
reused; this delivery does not establish full POSIX assignment semantics. Phase C
also rejects oversized variable values instead of silently truncating them.

The inherited `$$` behavior still expands the existing fixed value rather than
the runtime PID; do not use it for unique filenames or identity. This limitation
predates scripting and is not a concurrency primitive.

Physical console/SSH script acceptance, integration with Watchtower's updated
shell shutdown handlers, installed-root packaging acceptance, and a new USB
image remain separate. Issue #14 stays open for the later language phases.

## Phase C follow-up (2026-10-18)

Phases A–B were committed as `1978c58` on `codex/shell-scripting`. Phase C is
implemented on top, separately from the main checkout's Watchtower delivery.

The shared control compiler validates a complete unit into bounded instructions
before executing any part of it. Its source, token records, instructions and loop
frames are owned BSS. The runtime is an iterative program-counter loop; expansions
and command parsing cannot overwrite its structural state. The compiler's
recursive list parsing is capped at eight levels.

Supported grammar: `if/then/elif/else/fi`, `while/do/done`, `for name in words`,
implicit `for name` over positional arguments, and `break [levels]` /
`continue [levels]`. Loops re-expand body/condition commands every iteration;
for lists expand once on entry and preserve empty and spaced quoted arguments.
Empty for lists and loops with no executed body return zero. Conditions use
ordinary command statuses and can contain simple pipelines and `&&`/`||` lists.
Both interactive input and scripts use the control compiler.

`test` and `[` are shared child-safe builtins. The fixed-arity subset supports
empty/nonempty strings, `-n`/`-z`, string `=`/`!=`, `-e`/`-f`/`-d`, signed
64-bit `-eq`/`-ne`/`-lt`/`-le`/`-gt`/`-ge`, and a single leading `!`.
`[` requires `]`. Unsupported/ambiguous forms, malformed integers, overflow and
unexpected stat errors return 2; false expressions return 1. No `-a`/`-o`
boolean expression parser is implied. Existing `true`/`false` builtins are reused.

Bounds: 16 KiB per compound unit, 4096 bytes per simple command, 1024 lexical
tokens, 256 instructions, eight nested control levels and eight loop frames.
Each for frame owns at most 4096 value bytes; values must fit the shell's
255-byte variable bound. No arbitrary iteration limit is imposed. Signal checks
between instructions and the existing foreground wait path handle cancellation.

For this initial grammar, compounds require semicolon/newline separation;
compound pipeline stages, operators/redirections attached after `fi`/`done`,
and asynchronous compounds are rejected. Simple pipelines/redirections/background
commands remain usable within bodies. `until`, `case` and functions are deferred.

```sh
for host in example.com 192.168.0.1; do
    if ping "$host"; then
        echo "Reachable: $host"
    else
        echo "Unreachable: $host"
    fi
done
```

Verification after Phase C: the shell, pipeline, job-control, builtin-dispatch
and standalone-runner host suites pass under ASan/UBSan. New actual-code tests
cover branch selection, loop expansion and status, nested transfers, invalid
syntax before side effects, all structural budgets, cancellation, integer bounds,
file predicates and variable overflow preservation. Parser regressions now
explicitly distinguish incomplete trailing pipes from invalid operators and
accept newline/comment continuation after pipeline/list operators.

The guest matrix expanded to **184/184 PASS** (46 per BIOS/UEFI × SMP=1/4),
including the A–B regression cases, real branches/loops, `test` in a pipeline,
implicit for arguments, missing bracket errors, interactive control flow and
Ctrl+C during a builtin-only infinite loop. These checks use temporary ISOs
with the new shell and builtin runner, no data disks, and preserve the base
release inputs. The JSON report now describes this latest matrix rather than
the earlier A–B run. Assertions remain selected output/status checks.

No merge, push or USB image build was performed for Phase C. Physical script
acceptance and integration with Watchtower's committed shutdown handlers remain
separate. Phase D follows below.

## Phase D follow-up — source and return

2026-10-18. Implemented and verified in the same isolated worktree. A–B remain
committed as `1978c58`; C and D were subsequently committed as `41ef112`. No main-checkout integration,
push, release image rebuild, or physical acceptance is claimed.

`source file [args...]` and `. file [args...]` read a regular file in the current
interpreter. Paths are opened as given (relative to cwd when appropriate),
without PATH search. Variables, exported values and cwd changes affect the
caller. `$0` stays unchanged. Explicit source arguments temporarily replace
positionals and are restored after EOF, return, read/parse errors or cancellation.
Without explicit arguments, positionals are shared, including `shift` changes.
Arguments retain the 31-position/1024-byte positional storage limits; excessive
arguments fail before publication and leave the caller's parameters unchanged.

`return [status]` leaves the current sourced file, including nested control flow.
Status defaults to `$?`; explicit values must be decimal 0..255. Invalid operands
return 2 without leaving the file. Return outside a sourced file returns 2.
The first command inherits the caller's `$?`, even after blank/comment lines;
an otherwise empty or comment-only file returns zero.
An explicit return status survives a local `! return` prefix. The caller can
negate the completed source normally. Source returns its last command status;
an empty file returns zero. `exit` still exits the interpreter. Source/return
are parent-only builtins and are rejected as pipeline stages/background commands.
Break/continue cannot cross a source boundary into a caller's loop.

```sh
source /home/operator/settings.sh "first argument" second
echo "Source status: $?"
. ./helpers.sh
```

The source scheduler is iterative. A request yields through the simple-command,
command-list and control interpreter layers before the next source runs. Nine
stable BSS slots retain the root caller and eight source levels: AST pools,
reader buffers, program counters, loop values, list cursors, parameter snapshots,
temporary variable scopes and redirection saves never alias an active caller.
Compiler scratch is reused only after unwinding; compiler recursion remains
capped at eight control levels. The runtime adds no C recursion or heap use.
The ninth nested source is rejected without opening another file. Normal loop
iteration remains unbounded and interruptible.

Readers use separate CLOEXEC descriptors duplicated into available slots 20..31;
stdin remains available for sourced child commands. EOF, explicit return, syntax
or read failure, depth rejection and signals unwind source descriptors and
caller redirection/temporary-assignment scopes. Explicit overwrites or duplication
of live interpreter reader/suspended-save descriptors are rejected. Descriptor
pressure can reject a source earlier than the depth limit, with cleanup. A
redirection bug found during this work is also fixed: saved descriptors now skip
all fds mentioned anywhere in the command, so a later `20>&-` or `2>&20` cannot
overwrite or accidentally borrow the saved stdout copy.

Verification:

- `make build/shell.elf build/sh-builtin.elf test-shell-scripting-host`: PASS.
  Actual-code ASan/UBSan coverage includes suspended control-slot resumption,
  loop/list preservation, reader isolation/CRLF/EOF/error paths, positional
  snapshot restoration after shift/replacement, high-fd redirection collisions,
  and existing shell/pipeline/job/builtin/runner regressions. Syscalls in host
  orchestration tests remain mocked.
- `python3 scripts/test_shell_scripting.py --base /mnt/c/Sources/FortressOS`:
  **332/332 PASS**, BIOS/UEFI × SMP=1/4, 83 cases each. Includes A–C regressions,
  shared variables/cwd, spaced/empty source arguments and `$0`, shared versus
  restored shifts, nested caller/source loops, exactly eight source levels and
  ninth-level rejection/recovery, return/default/error/negation/exit statuses,
  `&&`/`||` resumption, missing/directory/incomplete/malformed files, source
  redirection/stdin and scoped assignments, argument-limit preservation, 64
  repeated sources without descriptor exhaustion, parent-only rejection,
  interactive source, nested cancellation and a successful source afterward.
  Assertions check selected output and statuses; they are not a full independent
  POSIX language oracle.
- Final tested shell SHA-256:
  `da1bfd924b1a8e0bb4ef92f029e093959564cc2d6e08bab231f939e751bd3bc6`.
  Report: ignored `build/shell-scripting-result.json`.
- GCC stack usage: runtime `run_control` 96 B, `control_execute` 96 B,
  `control_simple` 96 B, `script_reader_next` 64 B, `source_finish` 32 B.
  Largest existing shell frame is 912 B. Source depth does not multiply these
  runtime C frames. Shell BSS is 2,526,396 bytes (fixed capacity, not heap growth).
- Fixtures use temporary disposable ISOs/OVMF vars and no data disks. Primary
  IMG, ISO, initramfs and kernel hashes remain unchanged. No private keys or
  secret-bearing disks are generated or retained. Watchtower/concurrent primary
  work was neither edited nor committed.

Remaining: merge/reconcile with the current main checkout, explicit physical
console/SSH scripting acceptance, and any later language designs (functions,
substitutions, subshells, here-documents, traps, arithmetic and direct text
execution). Issue #14 must retain these subset and delivery boundaries.

## Follow-up — until loops

2026-10-18. Added after the C–D commit `41ef112` and delivered in the subsequent
`until` commit. `until condition; do body; done` evaluates the condition before
each iteration, runs the body on nonzero status, and stops on success (zero).
It uses the existing while-loop frame and an inverted condition branch,
including last-body status (zero for no iterations), nested break/continue,
source suspension/resumption and signal checks. The existing eight-level
control nesting, source/input/token/instruction/value bounds are unchanged.
Compounds still require newline/semicolon separation; unsupported attachments
after list operators, pipelines or compound redirections remain rejected.

`make build/shell.elf build/sh-builtin.elf test-shell-scripting-host` passes.
Actual-code sanitizer additions cover zero iterations, a changing condition,
nested break/continue, incomplete/malformed input, unsupported list attachments
and cancellation. The guest runner adds real interactive and script `until`,
source in its body, multi-level continue, syntax-before-effects, and Ctrl+C
with status 130. An initial guest test had overescaped quotes and consequently
kept its condition false; that fixture was corrected before final acceptance.

Final `python3 scripts/test_shell_scripting.py --base /mnt/c/Sources/FortressOS`:
**364/364 PASS**, BIOS/UEFI × SMP=1/4, 91 cases each. This supersedes the Phase D
JSON report with the expanded regression matrix. Final shell SHA-256:
`8c8e40730bfd92f6bffba0f3e06bde044514d437d7e71d42a4f32fb5e851c8d3`.
Disk-free temporary fixtures and unchanged primary release-input hashes retain
the same evidence boundary; no merge, push, release-image rebuild or physical
acceptance. `case`, functions, `read` and the other larger language features
still need their own bounded designs.
