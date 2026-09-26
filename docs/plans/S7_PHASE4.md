# S7 Phase 4: pipeline executor implementation preparation

**Status: implemented; user-run acceptance pending.** The approved design below
is retained as the contract. See [implementation and verification handoff](../roadmap/shell-s7-phase4.md).
Baseline: Phases 1–3 complete,
including commit `fdcf13244a99ce1c0c032a7ee14a9e4db5f3ee24`.

## Scope and current code

Keep the flat `parse_cmd_t[]` and eight-stage limit. At the Phase 3 baseline,
`execute_parse_tree` rejected pipes; `spawn_program` immediately waited after spawning, so it could not
be reused unchanged inside a pipeline. `SYS_PIPE`, blocking stream callbacks
and clone/actions/CLOEXEC sweep already exist. No syscall or scheduler redesign
is planned here. Current peers must stay on the BSP because wake channels are
CPU-local. **Phase 4 is BSP-only.** Cross-core pipe implementation and verification
are Phase 6 scope. Phase 4 delivers execution for external programs; builtin-stage
support and stream utilities are Phase 5 work. It does not yet deliver examples
such as `echo hello | wc -l`.

### Verified Phase 2 prerequisite

Source inspection confirms `process_spawn_internal` sets
`p->fd_flags[act->dst_fd] = 0` after the spawn DUP2 action's optional duplication
block, including when source equals destination. `fd_close_cloexec(p)` follows
the actions and precedes child publication. `tests/pipe_user.c` covers 3→0 and
3→3 actions, explicitly asserts `F_GETFD == 0` in the child, and checks that
unused CLOEXEC ends are absent. This prerequisite is implemented; no kernel
change is required. This review inspected source and tests; it did not rerun them.

## Execution rules

1. Starting at command index `first`, scan consecutive `CMD_OP_PIPE` entries to
   find `last`. The connector on `last` joins this whole pipeline to the next
   group. Evaluate `&&`/`||` with the preceding group's status; skip all stages,
   expansions and redirections in a skipped group. `;` starts the next group
   unconditionally. Preserve the existing single-command path.
2. For a successfully launched pipeline, collect every PID and return the last
   stage's status. There is no pipefail mode. Apply leading `!` once to the
   whole pipeline, rather than to its first stage.
3. Prepare bounded argv, environment and redirection data before launch. Keep
   command-local assignments scoped to their stage and restore parent variables.
   Expansion errors and action-capacity errors must reject before any spawn or
   destructive open. Preserve quoted targets and ambiguous-redirection errors.
4. Create N-1 pipes with `VFS_O_CLOEXEC`. Append each stage's stdin/stdout DUP2
   actions first, then its explicit redirections in lexical order. This lets
   `a >file | b` override the pipe output and gives `b` EOF after unused writer
   copies close. Let the existing kernel sweep remove unused CLOEXEC ends.
5. Launch every stage without waiting. Close all parent pipe copies before the
   first wait. Launching and waiting one stage at a time deadlocks on a full pipe.
   Separate the existing spawn helper into launch and wait operations while
   preserving its ordinary-command behavior and error mapping.
6. On pipe creation failure, close all ends already created. On spawn failure,
   save the mapped launch-failure status, close all parent pipe ends immediately,
   then call blocking `SYS_WAIT(pid, &child_status)` for every successfully
   launched PID in launch order. Collect statuses for cleanup but do not replace
   the saved failure with them. Return only after those children are reaped;
   this is not detach-and-forget. Attempt the remaining waits if one wait fails,
   report the wait failure, and preserve the original launch error. No kernel
   wait deadline is implied: completion depends on cooperative child termination;
   host test timeouts bound the verification, not arbitrary guest execution.

## Executor loop and guard replacement

Replace the current per-command `while` loop with group traversal. The incoming
connector belongs to the previous group's last command, not the current group.
For example, `false && a | b ; c` skips both a and b, retains the false status,
then runs c because the skipped group's outgoing connector is a semicolon.

```c
int status = (int)last_status;
enum cmd_op incoming = CMD_OP_NONE;
for (int first = 0; first < tree->cmd_count; ) {
    int last = scan_pipeline_end(tree, first);
    enum cmd_op outgoing = tree->cmds[last].next_op;
    bool run = incoming == CMD_OP_NONE || incoming == CMD_OP_SEMI ||
               (incoming == CMD_OP_AND && status == 0) ||
               (incoming == CMD_OP_OR && status != 0);
    if (run) {
        if (first == last) {
            status = execute_single_command(&tree->cmds[first], status);
            /* Existing single-command helper owns its negation semantics. */
        } else {
            /* Preflight, launch all, close parent ends, wait all. */
            status = execute_pipeline(tree, first, last, status);
            if (tree->cmds[first].negate) status = status == 0 ? 1 : 0;
        }
    } /* Skipped groups retain status and do not expand or allocate anything. */
    incoming = outgoing;
    first = last + 1;
}
last_status = status;
```

Helper names above are proposed, not existing APIs. Extract single-command
behavior without changing its assignments, builtin effects or redirection scope.

Delete `parser_execution_guard()` from parser.c/parser.h and remove its executor
call once pipeline execution is ready. Do not repurpose that parser API.
Introduce executor-local `pipeline_preflight()` called only for a selected
multi-stage group, before pipe allocation, any spawn or destructive open. It
rejects builtin stages, assignment-only/redirection-only or expansion-empty
stages, non-leading negation, expansion errors and combined action overflow.
Quoted names must be classified after expansion. Replace the blanket-rejection
tests with execution tests and unsupported-stage preflight canaries. A skipped
group is not preflighted; ordinary parser syntax errors still reject the input.

## Diagnostics and shell status

Preserve the existing shell error model rather than converting every failure to
1. Emit executor diagnostics on stderr once at the failure site. Values below
are the raw group result; a supported leading `!` logically inverts that result.
Parser failures occur before execution and are not negated.

| Failure/result | Diagnostic | Status |
| --- | --- | --- |
| SYS_PIPE or relocation EMFILE | `Too many open files.` | 1 |
| SYS_PIPE ENOMEM | `Out of memory or process capacity.` | 1 |
| Other pipe/relocation failure | `Unable to create pipeline.` | 1 |
| Spawn ENOENT | `No such file or directory.` | 127 (existing behavior) |
| Spawn ENOEXEC | `Invalid executable.` | 126 |
| Spawn EISDIR / EFBIG | Existing `Not a regular file.` / `Executable exceeds 4 MiB limit.` | 126 |
| Spawn EROFS / EIO | `Read-only filesystem.` / `I/O error.` | 1 |
| Spawn ENOMEM / E2BIG | Existing memory/process-capacity / argument-list diagnostic | 1 |
| Spawn EBADF / EINVAL | Existing bad-descriptor / invalid-spawn-arguments diagnostic | 1 |
| Other spawn failure | `Unable to load executable.` | 1 |
| Unsupported stage | `pipeline: unsupported stage` | 1 |
| Non-leading negation | `pipeline: negation is only supported before the first stage` | 1 |
| Combined action count >16 | `pipeline: too many spawn actions (max 16)` | 1 |
| Expansion/ambiguous redirect | Preserve the specific existing diagnostic | 1 |
| Checked expansion capacity loss / spawn vector bounds | `Expansion exceeds shell limits.` / `Argument list too long.`; before any launch | 1 |
| Wait failure with no prior launch error | `Unable to wait for child process.`; still attempt other waits | 1 |
| Nine-stage pipeline | Existing parser diagnostic `pipeline too long (maximum 8 stages)` | 2 (existing parser-error behavior) |
| All stages launched and waited | No synthetic failure for an earlier stage's exit | Last stage's exit status |

After partial launch failure: **close pipes → SYS_WAIT all launched PIDs → return
saved launch-failure status**. An EPIPE/141 exit observed during cleanup never
overwrites that failure. No SIGPIPE or SYS_KILL is introduced; both remain S8 work.

## Bounds and descriptor ownership

- Use BSS for stage preparation, action arrays, target strings, PIDs and pipe-fd
  tracking. Never put command or stage arrays on the 4 KiB user stack. Inspect
  compiler stack-usage reports with the existing 512-byte headroom requirement.
- `MAX_SPAWN_ACTIONS` is 16. An interior stage needs two pipe DUP2 actions, leaving
  at most 14 explicit actions. Check the combined count before launch; do not
  silently truncate actions or enlarge the syscall ABI in this phase.
- Do not assume `SYS_PIPE` returns fds >= 3. If standard descriptors are closed,
  relocate pipe sources with `F_DUPFD_CLOEXEC` to slots >= 3 before building
  actions, closing originals and unwinding on failure. Otherwise a DUP2 can
  overwrite another pipe source. Account for temporary relocation capacity.
- Retain the shell's CLOEXEC terminal fd 31. Never apply pipeline redirections
  to the parent. Track ownership explicitly so every acquired fd closes once.
- Spawn copies user argv/actions synchronously, but static preparation buffers
  must remain valid until that stage's spawn returns. Avoid shared-buffer pointer
  aliasing when preparing several stages ahead of launch.

## Pinned scope decisions

- **Builtin stages:** initially support external executables in multi-stage
  pipelines; reject builtin, assignment-only and redirection-only stages before
  allocating pipes or running any stage. Preserve all existing single-command
  builtin behavior. Running builtins in the parent can block before consumers
  launch and can mutate shell state. A child builtin runner would be additional
  scope. `echo` and current `cat` are builtins, so test with dedicated external
  fixtures until Phase 5 provides builtin-stage support and stream utilities.
  `hello` and `dual_stream` are existing producers; `dual_stream` only writes
  stdout/stderr and is not a sink. Add dedicated stdin-reading relay/sink and
  cooperative early-exit fixtures for meaningful streaming and teardown tests.
- **Negation after a pipe:** the parser can represent `a | ! b`. Reject that
  form during group preflight; support `! a | b` as whole-pipeline negation.
  Do not silently reinterpret per-stage negation.
- **Failure termination limits:** closing endpoints releases children blocked
  on these pipes, provided they handle EOF/EPIPE. It cannot terminate an arbitrary
  child that ignores pipe I/O, loops forever, or has redirected away from the
  pipe. There is no SYS_KILL. Tests must use cooperative fixtures and bounded
  host timeouts; do not claim general cancellation or bounded prompt recovery.

## Implementation order

1. Extract launch/wait helpers; preserve non-pipeline regressions.
2. Add group traversal and preflight with BSS storage and combined action bounds.
3. Add pipe allocation/relocation, ordered actions and all-stage launch.
4. Add parent closure, failure cleanup, complete reaping and status propagation.
5. Delete the Phase 3 parser guard and its call only once the above paths exist;
   use executor-local `pipeline_preflight()` for selected multi-stage groups.
6. Add host orchestration tests and a BIOS/UEFI integration runner, then update
   evidence and remove obsolete interim-guard tests in favor of execution tests.

## Acceptance to implement (not yet run)

| Gate | Required checks |
| --- | --- |
| Grouping/status | `a\|b && c\|d`, `a && b\|c`, `a\|b; c\|d`, OR, negation, skipped groups with no side effects; reap every stage, report last-stage status |
| Streaming | External producer/relay/sink fixtures; 2, 3 and 8 stages; >256 KiB byte comparison; short writes and EOF |
| Redirections | Pipe DUP2 before lexical explicit redirects; stderr ordering; closed 0/1; fd 31 preservation; no parent stream changes |
| Bounds/failure | 16-action combined limit, fd exhaustion, pipe allocation failure, first/middle/last spawn failure; cooperative teardown, exact fd/refcount/process cleanup |
| Early reader exit | Producer handles SYSCALL_EPIPE (already negative), exits 141; shell still returns the last stage status; do not classify 141 automatically as a hardware fault |
| Regression | `make test-host`, `make test-pipe`, `make test-shell-s6`, `make test-shell-s6-resources`; retain single-command and spawn CLOEXEC behavior |

Proposed new target: `make test-shell-s7` with disposable fixtures, paired OVMF
code/vars, bounded waits, unconditional QEMU cleanup and retained logs. A
multi-CPU VM still running all pipe peers on the BSP is not cross-core acceptance.
The user will run tests; this document records no new runtime verification passes.
