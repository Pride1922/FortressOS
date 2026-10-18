# Shell scripting — issue #14

**Status:** PHASES A–D IMPLEMENTED IN ISOLATED WORKTREE (2026-10-18). See [evidence and delivery limits](../roadmap/shell-scripting.md). Not merged or included in release media.

**Date:** 2026-10-18
**First delivery:** explicit `sh file [args...]` and `sh -c command [name [args...]]`.

This plan refines the scripting increments in [SHELL_DESIGN.md](SHELL_DESIGN.md#s9--scripting-in-useful-increments).
The existing shell milestone names are historical: the introspection milestone
also uses S9. Use the phases below for this work. Implementation status belongs
in [the shell subsystem guide](../subsystems/shell.md); this is a design proposal.

## 1. Goal and existing foundation

Make repeatable administration possible without typing every command. Start
with script files and arguments, then add structured control flow. Reuse the
interactive shell's parser, expansion, redirection and execution machinery.
This is a documented FortressOS shell subset, not a POSIX or Bash compatibility claim.

Observed in the current source:

- `user/shell.c` ignores entry argc/argv and always initializes terminal UI,
  history and MOTD before entering its prompt loop.
- `user/shell_start.asm` discards the shell result and exits with status zero.
- `parser.h` already represents newline/semicolon lists, `&&`, `||`, `!`,
  pipelines, redirections and background operators.
- `vars.c` and `expand.c` already implement variables, assignment handling,
  exported environments, quoting, globbing, `$?` and `$$`.
- Parser/expander scratch is static and reused between commands; nested
  invocation must not retain pointers into overwritten scratch.
- Job control and spawn-based execution already exist. Scripts must not acquire
  interactive terminal ownership merely to run commands.

Read PROTECTED.md, AGENTS.md §4/§7.6/§9, the shell subsystem guide and the
parser/expansion/job-control headers before implementation. No kernel ABI,
signal-delivery, scheduler, storage-gate or SSH protocol changes are planned.

## 2. Phase A — noninteractive execution

Package `/bin/sh` as the same shell implementation, retaining `/bin/shell` for
existing login callers. Dispatch modes before initializing interactive UI:

| Invocation | Behavior |
| --- | --- |
| `/bin/shell` or `sh` with a terminal on stdin | Existing interactive behavior |
| `sh file [args...]` | Read commands from a separately opened script descriptor |
| `sh -c command [name [args...]]` | Run supplied command text |
| `sh` with nonterminal stdin | Read commands from stdin until EOF |
| `sh -- file [args...]` | Allow filenames beginning with `-` |

Reject unsupported options with status 2. No automatic startup files, aliases,
history expansion, prompts, screen clearing, MOTD or history writes in script
mode. Import the existing bounded environment under the existing secure-entry
policy. Scripts never gain privileges from their file owner or mode.

Extract shared initialization and command-unit execution; keep the interactive
read/render loop separate. Existing shell builtins and external commands keep
their semantics. `cd` and assignments persist within the script interpreter.

Read via bounded chunks and handle short reads. Do not use the terminal input
syscall for file scripts. A file-script descriptor is CLOEXEC and separate from
fd 0, so child programs inherit the caller's stdin rather than script contents.
For stdin scripts, use a non-read-ahead policy and document that commands reading
stdin consume that same stream; callers should redirect their input explicitly.

Support LF, CRLF, blank lines, a final line without newline, comments beginning
at an unquoted token boundary, and quoted literal `#`. Never strip comments or
split commands with a raw text search: use lexer state. Backslash continuation
and quoted multiline units use the existing incomplete-parse results. EOF in
an incomplete unit is an error; never show a continuation prompt in script mode.

Return the final command status, or zero for an empty script. `exit [0..255]`
exits the script, invalid operands return 2, and excess operands diagnose without
silently exiting. Preserve 126 for an unexecutable command, 127 for not found,
and 128+signal for signal termination where the existing executor supports them.
Invocation, syntax, input-bound and script-read errors return 2 with diagnostics.
Update the C entry and assembly stub together so the actual status reaches
SYS_EXIT. Ordinary nonzero commands do not abort the script; there is no `set -e`.

On exit or cancellation reuse the existing child cleanup policy with bounded
cleanup; do not leave new background descendants behind. Noninteractive jobs
must not print prompt-oriented notifications. Audit jobs, pipes and signal
initialization separately from terminal setup before sharing the executor.

**Deliverable:** explicit script and `-c` execution; child status propagation;
the existing interactive shell still behaves as before.

## 3. Phase B — positional parameters

Provide `$0`, `$1` through `$9`, `${10}` through `${31}`, `$#`, `"$@"`, and
`shift [n]`. Use a separate bounded positional-argument context, not ordinary
variable names. Missing parameters expand empty; unset ordinary variables retain
the existing semantics. Invalid or excessive shifts return nonzero without
changing the context. Zero shifts succeed.

For file mode, `$0` is the supplied script path. For `-c`, the optional name
sets `$0`, followed by positional arguments; without a name use `sh`.
Quoted `"$@"` produces one argument per parameter, preserving empty arguments,
and zero arguments when no parameters exist. Specify prefix/suffix joining for
forms such as `"pre$@post"` before coding and cover zero/one/many arguments.
Do not implement quoted `$@` by joining and splitting strings. Unquoted
expansions use the existing field-splitting/globbing rules. Unsupported special
parameter forms must diagnose rather than silently produce misleading output.

Example after Phases A–B:

```sh
# Run: sh /home/operator/check.sh example.com
HOST="$1"
echo "Checking $HOST"
ping "$HOST"
echo "ping status: $?"
```

**Deliverable:** useful parameterized scripts, without control-flow syntax.

## 4. Phase C — conditions and loops

Implemented with an explicit 16 KiB source-unit ceiling, 1024 tokens, 256
instructions, nesting depth eight, and at most 4096 bytes of expanded for-list
storage per loop frame. Runtime interpretation is iterative; only the compiler
uses depth-bounded recursion. Each simple command still has the 4096-byte limit.
Compounds are separated by semicolons/newlines; compound pipeline stages,
`&&`/`||` attachments to compounds, compound redirection and asynchronous
compounds are rejected. Simple commands/pipelines inside conditions and bodies
retain their existing operators. Supported in script and interactive modes.

Add `if/then/elif/else/fi`, `while/do/done`, `for/in/do/done`, `break`,
`continue`, and a bounded `test` / `[` builtin. Reserved words are recognized
only in grammar positions; quoted text and arguments remain ordinary words.
Parse and validate each complete compound unit before any of its commands run.
Conditions execute command lists and inspect status; they are not a second
expression language. Define pipeline and conditional-list status consistently.

Start test expressions with argument-count rules for empty/nonempty strings,
`=`, `!=`, `-n`, `-z`, signed integer comparisons, and file existence/type.
Reject ambiguous boolean-expression extensions initially. `[` requires its
closing `]`. Invalid operands return 2. Bound integer parsing and reject overflow.
Add `true` and `false` if the existing command set lacks them.

Keep structural program state in bounded storage outside reusable command
scratch. Re-expand commands on every loop iteration. No arbitrary iteration
limit for intentional loops: honor signals between iterations and while waiting
for children, and ensure Ctrl+C stops the foreground script cleanly.

**Deliverable:** scripts can branch, retry with an explicit sleep, and iterate
over fixed argument lists. Functions, `case` and arithmetic remain later work.
The subsequent `until` increment reuses the while-loop compiler/runtime, with
the condition exit branch inverted: the body runs on a nonzero condition status
and stops on zero. It retains the same nesting/size limits, body status,
break/continue, source suspension and cancellation behavior. See follow-up
evidence in the scripting roadmap.

## 5. Phase D — source and later language features

Implemented with an iterative source driver and nine stable BSS slots (caller
plus eight sourced files). Source yields unwind the C executor stack before
another file runs. Caller ASTs, command-list cursors, loop PCs, temporary
assignments and redirection saves remain owned by their original slots. The
compiler alone retains depth-bounded recursion. No recursive runtime execution
is introduced. See the Phase D evidence and semantics in the roadmap.


Add `source`/`.` and `return` only after scoped execution contexts can preserve
input state, positional parameters and caller control flow across nested loads.
Bound source depth to eight; unwind descriptors and contexts on every error.
Sourced variable/cwd changes affect the caller; positional arguments supplied to
source are restored on return. A sourced file may not overwrite an active parse
tree through shared static scratch.

Functions, command substitution, subshell groups, here-documents, traps,
`set -e`, arithmetic and direct shebang execution are separate later designs.
Do not change the ELF loader to execute text for this initial delivery. Use
`sh script` explicitly. Watchtower service definitions continue using their own
strict Exec argv schema; this work does not introduce shell evaluation there.

## 6. Bounds and error contract

- Existing `LINE_CAP=4097`: at most 4096 bytes per complete command unit,
  including accumulated continuations. Reject overflow without truncation or
  executing a partial unit. A script may contain many such units.
- Preserve current parser limits: 32 commands, 64 raw args, 16 redirections per
  command and eight pipeline stages; spawn and expanded-argument limits remain
  separate and must be checked before side effects.
- Preserve the 64-variable table and 255-byte value limit. Reject oversize values.
- At most 31 positional arguments initially, checked against the real spawn ABI
  including interpreter argv. Store their bounded bytes in owned static storage.
- Phase C: compound nesting at most eight; source 16 KiB, tokens 1024,
  instructions 256, eight loop frames with 4096 bytes of for-list values each.
- Use BSS for large parser/input contexts; no large arrays or recursive C
  interpreter frames on the user stack. No new dynamic allocation is required.
- Diagnostics identify the input name and 1-based line/column for lexical/syntax
  errors; runtime errors include command context without dumping environments.
- No automatic execution from USB, account profiles, SSH startup, or Watchtower.

## 7. Implementation map and acceptance

Likely touchpoints: `user/shell.c`, `user/shell_start.asm`, new
`user/shell/script.c/.h`, lexer/parser/expand/vars/builtins, and Makefile packaging.
Inspect `program.c`, pipeline and job-control callers before executor changes.
Coordinate any shared shell shutdown changes with the Watchtower work; do not
modify its in-progress files as part of this feature.

For each phase, add focused actual-code host coverage where parsing or argument
semantics are subtle, then bounded BIOS/UEFI integration with disposable fixtures.
Tests are planned, not executed by creating this document. Reuse the existing
shell test runner; add a scripting runner only where guest orchestration differs.

| Gate | Required evidence |
| --- | --- |
| Phase A | File, stdin and `-c`; LF/CRLF; short reads/read errors; comments/quotes; incomplete EOF; no UI/history side effects; returned status visible to caller |
| Phase B | Empty/spaced/quoted args, `$0`, `$#`, `$?`, quoted `$@` cardinality, shift bounds; environment and argument overflow rejection |
| Phase C | Nested branches/loops, condition statuses, per-iteration expansion, syntax rejection before compound side effects, Ctrl+C and child cleanup |
| Phase D | Nested source depth eight; caller lists/loops survive; shared vars/cwd; explicit argument restoration; shared no-argument shifts; early return; fd/redirection/error/cancellation cleanup |
| Regression | Interactive prompt/edit/history, sudo, redirection, pipelines, foreground/background jobs, ordinary unpinned execution |
| Isolation | No descriptor/child leaks; no script fd inherited; stdin remains usable by child commands; no writes to non-target media |
| Later physical check | Explicit laptop script over console and SSH; cancellation returns control cleanly. QEMU cannot establish physical behavior |

Record evidence per completed phase. Do not close #14 for a file runner alone
without stating its supported subset and the remaining language phases.

## 8. Recommended delivery order

Implement A, then B, review the resulting command/input/status contracts, then
implement C. Phase D is independently scheduled. Source implementation was
subsequently authorized for A–B in a separate worktree. Release image builds,
commits and GitHub issue updates remain separate.
