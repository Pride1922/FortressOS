# S7 Phase 5B: builtin pipeline stages

**Status: reviewed and approved with the refinements below; not implemented.** Prepared 2026-09-27 after
[Phase 5A acceptance](../roadmap/shell-s7-phase5a.md). User owns test execution.
This checkpoint proposes a bounded allowlist of child-safe builtins, not full
POSIX subshell semantics. Phase 6 still owns cross-core and hardware acceptance.

## Result and scope

Support `echo hello | wc -l`, `pwd | cat`, and `view FILE | wc -c` by launching
each builtin stage as its own process through the existing `SYS_SPAWN_EXT` path.
All stages launch before any wait. Standalone builtins continue to run in the
parent with existing scoped redirection. Never run a producer in the parent
while its reader is waiting to be launched: output can exceed pipe capacity.

| Classification | Commands and semantics |
| --- | --- |
| Supported child stages | `echo`, `pwd`, `true`, `false`, `env`, `help`, `version`, `ls`, `view`, `type` |
| Rejected before pipeline side effects | Every other builtin, including `cd`, `exit`, `set`, `unset`, `export`, `alias`, `unalias`, `history`, `prompt`, `terminal`, `layout`, `edit`, `run`, `command`, filesystem mutation and power commands |
| Unchanged | External commands, including `/bin/cat`, `/bin/head`, `/bin/tail`, `/bin/wc`; empty/assignment-only stages remain rejected |

Use an explicit shared capability table, default deny for future builtins. For
this checkpoint even `command echo ... | ...` is rejected; wrapper unwrapping
and child-local state-changing builtins are deferred. A rejected stage returns
1 with a specific diagnostic naming the builtin. `echo ok > canary | cd /`
must neither open canary nor create pipes nor spawn children. Rejection in a
short-circuited group performs no work and produces no diagnostic.

No fork, shell-state serialization, recursive parsing, new kernel API, new fd
transport, scheduler changes or changes to the flat parser/eight-stage bound.
Parent variables, aliases, history and prompt cannot be changed by a stage.
This is a proposed bounded resolution of the master plan's builtin-stage item;
document the remaining exclusions in help and at acceptance.

## Current code and chosen architecture

`user/shell/pipeline.c::prepare_stage()` currently rejects every builtin after
expansion. It already copies each stage's argv/environment out of shared arenas,
then builds pipe wiring before lexical redirections. Retain that ownership and
ordering. `user/shell.c::execute_simple_command()` contains the builtin bodies;
several output helpers return void or ignore write errors. Simply removing the
rejection or spawning the existing interactive shell is insufficient.

Add a dedicated `/bin/sh-builtin` ELF with an aligned entry stub and a bounded
`argc/argv/envp` entry point. It dispatches exactly one allowed command and exits
with its result. It never initializes the shell UI, runs stdin validation probes,
loads history, prints a banner, opens the retained terminal fd, parses text or
enters a prompt loop. Direct invocation is permitted and validates the same
allowlist; it must never provide a back door to power or interactive commands.

Use the original expanded builtin name as `argv[0]`, with original operands in
the remaining entries; select `/bin/sh-builtin` as the executable path separately.
The runner also accepts direct `/bin/sh-builtin NAME [ARG ...]` invocation by
normalizing its own invocation form. Do not add an internal argument to the
pipeline representation: it would reduce the existing 32-argument/1024-byte
argument budget. No command string is reparsed or expanded a second time.
Reject a missing/unknown/forbidden runner command with status 2 and bounded stderr
diagnostics. The normal shell preflight rejection remains status 1.

Before implementation, verify the loader's argv[0] independence and argc/argv/envp
register setup in the actual spawn/ELF code and fixtures. Use the existing ABI,
not assumptions from a hosted C runtime. Preserve all ABI constants:
32 args, 256 bytes per arg, 1024 total argument bytes; 32 env entries, 256 bytes
per entry, 1024 total environment bytes, plus existing stack-floor checks.

## Shared behavior and environment

Extract supported handlers into a small shared module, proposed
`user/shell/builtin_exec.c/.h`, linked by the parent shell and the runner.
Keep discovery/help in `builtins.c/.h`; do not maintain two independent copies
of echo/view/ls algorithms. The shared interface takes an explicit bounded
context for environment/PATH and returns an exit status. Do not link editor,
history or terminal UI objects into the runner merely to satisfy dependencies.

Runner link boundary: its entry/dispatcher, `builtin_exec.c`, `builtins.c`
(the full static command table, descriptions and help text), and the syscall/
status-bearing I/O helpers from `io.c`. Extract any required PATH lookup into
a context-driven shared helper rather than pulling in parent globals through
`program.c`. Environment lookup/iteration reads the supplied envp directly;
`vars.c` remains parent-side for checked snapshot construction, not a dependency
of the runner. Headers such as `syscall_abi.h` define the ABI, not link objects.
Do not link `shell.c`, alias storage, editor, history, prompt or terminal UI.
Verify the final object list and unresolved symbols during the build.

- Echo preserves the current operand spacing and final LF, including empty
  operands. `$?` and all other expansion happen in the parent exactly once.
- Pwd queries inherited process CWD. Type in the runner classifies only the
  runner allowlist as builtins, then resolves external names using its envp PATH
  and the existing lookup rules (including the `/bin` fallback). It has no parent
  aliases. Thus `type cd | cat` emits `cd: not found` when no external cd exists;
  the type child returns 1, while the pipeline still returns cat's status.
  If an external cd exists on PATH it reports that file. Standalone type keeps
  the full parent builtin table and alias lookup. Pass classification and optional
  alias lookup through the shared context; do not fork the formatting algorithm.
- Env prints the stage's exported environment, including temporary assignments,
  in the existing order. It does not synthesize defaults via `vars_init()`.
  Unexported variables are available to parent expansion but not runner env.
- View retains printable ASCII/tab/LF, dot substitution and the nonempty final-LF
  rule. It remains a text viewer; external cat remains byte preserving.
  The same shared implementation is linked into both binaries; there is no new
  `/bin/view`. A pipeline launches `/bin/sh-builtin` with argv[0] `view`.
- Help prints the full parent shell help, including topics outside the runner
  allowlist. `help cd | cat` documents cd without making it executable as a stage.
  `builtins.c` generates this text from static command metadata; descriptions of
  editor/history commands require no editor/history code. Add the same capability
  note to parent and child help: only the listed allowlist can execute in pipelines,
  and pipeline type reports that narrower execution context without parent aliases.
- Version/ls retain existing text and argument behavior. Ls uses inherited CWD
  and kernel path resolution, retaining current directory iteration order and
  formatting; do not add sorting or columns. Require byte-identical standalone
  and pipeline ls output for the same unchanged directory and CWD, including
  relative paths. Review path errors and return codes during extraction;
  stdout/stderr failures must propagate.
- True/false return 0/1 without reading stdin or emitting output.

Environment snapshots must be complete or fail preflight: `vars_build_envp()`
currently stops at 32 entries. Add a checked build interface (or equivalent
explicit overflow report) and use it for stage snapshots; do not silently omit
exported entries. Respect per-string and aggregate limits before any pipe/open/
spawn. Keep standalone call sites compatible or deliberately migrate them with
regression coverage. Restore assignment scopes on every preflight exit.

## I/O, lifecycle and error contracts

Shared handlers must stop at the first stdout failure. Use status-bearing output
helpers based on `write_bytes_fd()` or the utility runtime's equivalent, handling
short writes and zero progress. Avoid changing all shell UI output globally.
Return 141 quietly on stdout EPIPE, 1 on other I/O errors with best-effort fd 2
diagnostics, and never recurse if diagnostics fail. Owned inputs close on every
exit; inherited descriptors close through normal process exit. A read/close
failure cannot overwrite an earlier output failure.

Launch builtin stages at their lexical position through `program_launch()` with
the same fd actions as external stages: pipe DUP2 first, then user redirections.
Use the fixed runner path independently of PATH; absent/bad runner images use
normal launch error handling. Parent pipe copies close before waits. Reap every
launched child, preserve the original launch error on partial launch failure,
and retain last-stage status, pipeline negation and `&&`/`||` grouping.
Cleanup remains cooperative; this phase adds no cancellation guarantee.

All pipe ends remain CLOEXEC and the retained fd 31 remains excluded. Keep the
existing reserved-fd relocation and action budget. No extra environment pipe or
private fd is required. All peers remain BSP-only.

## Implementation sequence

1. Read PROTECTED.md, AGENTS invariants/recipes, syscall ABI, pipeline/program/
   redir/vars/io headers, loader argument code, and Makefile. Enumerate each
   supported handler's global/UI dependencies before extraction.
2. Introduce the shared capability table and status-returning handlers/context.
   This is extraction plus output-error conversion, not a mechanical move:
   convert `echo`, `pwd`, `type`, `env`, `help`, `version`, `ls` and `view` from
   void/unchecked output to checked writes, including spaces, numeric formatting,
   per-entry output and final newlines. View must stop reading after a failed
   write, close its owned input and preserve 141 on EPIPE. True/false need no
   output conversion. Test each emitting handler's first and later write failure.
   Route standalone supported builtins through them without changing parent
   redirection ownership. Preserve existing parent-only dispatch.
3. Add runner source/entry/build/package dependencies. Verify ELF permissions,
   freestanding flags, no red zone/SIMD, stack alignment and nested `.su` budgets
   against the existing 512-byte floor. Large buffers belong in BSS.
4. Extend stage preflight classification, checked environment capture and fixed
   runner path selection. Keep all expansion and unsupported-stage validation
   ahead of resource allocation. Reuse the existing launch/close/wait loop.
5. Update shell help, host expectations formerly rejecting supported view/echo,
   real integration fixtures, plans and roadmap. Keep negative cases by replacing
   old blanket rejection cases with explicitly unsupported builtins.

## Acceptance to author, then hand to the user

Extend `tests/pipeline_host.c` for actual parser/expander/executor classification,
argv/environment copying, scope restoration, no preflight side effects, missing
runner and injected pipe/spawn/wait failures with exact ownership accounting.
Add a host runner test linking the actual shared handlers with syscall mocks,
under ASan/UBSan; include it in `test-host`. Test short/zero writes, EPIPE, closed
stderr, input errors and forbidden direct invocation, not just successful strings.

Extend `scripts/test_shell_s7.py` and its fixture using the existing prompt parser,
bounded waits, BIOS/UEFI BSP configuration, disposable storage and offline audits:

- `echo hello | wc -l` => 1; pwd/env/type/help/version through cat or wc; compare
  view output against standalone view and retain binary cat checks.
- Capture standalone `ls DIR > A` and `ls DIR | cat > B` outside the listed
  directory; compare extracted bytes exactly. Cover default, absolute and relative
  paths under inherited CWD, files/subdirectories and empty directories without
  changing directory contents between captures. Compare error output/status for
  missing paths too; do not mistake the last consumer's status for ls's status.
- Compare complete help and topic output between parent and runner, including
  `help cd`. Verify the full help table cannot bypass runner dispatch restrictions.
  Test pipeline type for allowed echo, forbidden cd, external cat, an alias-only
  name and scoped PATH; verify parent type retains full builtin/alias discovery.
  Observe the type child directly or put type last when asserting its exit status.
- `false | true` => 0; `true | false` => 1; negation and conditional chains;
  supported builtins in first, middle and last positions and an eight-stage group.
- Redirect-over-pipe precedence and lexical `2>&1` ordering, closed stdout,
  assignment/PATH isolation and unchanged parent CWD/variables after failures.
- All forbidden builtins fail preflight, including a late forbidden stage with
  an earlier canary redirection; skipped groups cause no side effects.
- Exercise argument/env limits at and beyond bounds, literal metacharacters in
  expanded arguments, no second expansion, no banners or history/terminal activity.
- Make view read a large printable fixture so its output exceeds pipe capacity;
  verify completion with consumers launched concurrently and exact output offline.
  Echo's argv bound alone cannot exercise pipe backpressure.
- Extend the direct Ring 3 child-status observer for view into early-exit head:
  assert producer 141 and consumer 0, and view into draining tail zero => both 0.
  Shell last-stage status alone is not proof of the producer's result.
- Repeat mixed groups and launch failures; keep original cooperative cleanup,
  descriptor exhaustion, stream utility and canary regression cases.

Recommended user-run gates: `make`, `make test-pipeline-host`, the new runner host
target, `make test-stream-tools-host`, `make test-host`, `make test-shell-s7`,
`make test-pipe`, `make test-shell-s6`, and `make test-shell-s6-resources`.
Record only results actually supplied or run, with firmware/fixture scope.
Acceptance completes the bounded Phase 5B scope; stateful subshell semantics stay
deferred and Phase 6 must address existing cross-core restrictions before SMP
pipeline acceptance is claimed.
