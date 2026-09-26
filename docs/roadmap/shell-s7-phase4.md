# Shell S7 Phase 4: external pipeline executor

**Implemented 2026-09-26; user acceptance pending.** Phases 1–3 retain their
previous evidence. This change has build checks only, not new runtime passes.
Design contract: [S7_PHASE4.md](../plans/S7_PHASE4.md).

## Implementation

- `user/shell/pipeline.c` groups consecutive `CMD_OP_PIPE` entries in the existing
  flat array. Incoming `&&`/`||` applies to the whole next group; skipped groups do
  not expand or allocate. Leading `!` negates the pipeline once. The last stage
  supplies its status after every child has been waited for; there is no pipefail.
- Selected groups preflight all stages before pipe allocation or spawn. Builtins
  (including `run`/`command` wrappers), assignment-only, redirection-only,
  expansion-empty stages, non-leading `!`, checked expansion overflow and more
  than 16 combined actions are rejected. Explicit executable paths remain usable
  even when their basename matches a builtin. The parser's temporary guard is gone.
- Static per-stage storage owns argv strings, environment, paths, actions and
  PIDs. Local assignments are restored after preparation, including their original
  export flags. `expand_command_checked` detects truncated argument results;
  redirection expansion also rejects capacity loss instead of opening a truncated
  target. Ordinary command expansion retains its existing interface/behavior.
- All N-1 pipes use CLOEXEC. Private ends are relocated above standard fds and
  away from explicitly referenced redirection fds (and fd 31). This additionally
  prevents an otherwise closed user fd from resolving to a private pipe handle.
  Relocation may need a temporary spare fd and fails cleanly with EMFILE.
- Stdin/stdout pipe DUP2 actions precede explicit lexical redirections. Every
  stage launches before waiting; the kernel's existing CLOEXEC sweep removes
  unused ends. Parent handles close before any wait, on success and failure.
- `user/shell/program.c` separates launch, error mapping, PATH resolution and
  wait. Single-command execution retains its existing builtin/redirection path.
  A failed launch preserves its mapped error while waiting every earlier PID in
  launch order; a failed wait does not suppress later waits. Pipeline status 141
  is treated as a child status, with no synthetic hardware-fault diagnostic.

The existing spawn string/count limits now live in the public ABI header so
preflight can enforce per-string and total argv/env limits before any launch.
Their values and the loader's stack-floor assertion are unchanged.
No kernel ABI behavior, scheduler or synchronization changes. Existing process creation
pins children to the parent's CPU; shell pipe peers remain on the BSP. Builtin
stages/stream utilities belong to Phase 5, cross-core work to Phase 6. Closing
endpoints cannot stop a child that ignores EOF/EPIPE or runs forever; cancellation
and signals remain deferred. Partial launch cleanup is cooperative, not bounded.

## Checks performed

- `wsl -d Ubuntu-24.04 -- make build/shell.elf build/pipeline_fixture.elf` compiled
  and linked the freestanding executables with project warning/error flags.
- Forced compilation of `build/kernel/thread.o` and `build/kernel/syscall.o`
  after moving their existing spawn limits into the shared ABI header.
- Compiled (but did not execute) `tests/pipeline_host.c` and `tests/shell_host.c`
  with their real shell dependencies, ASan/UBSan and `-Wall -Wextra -Werror`.
- Python syntax compilation for `test_pipeline_host.py`, `test_shell_s7.py` and
  the shared S6 runner. `git diff --check` clean.
- Inspected compiler `.su` reports: the inlined pipeline/group executor frame is
  208 bytes; expansion's largest reported frame is 624 bytes. Stage arrays remain
  in BSS. These are frame-size observations, not runtime stack-depth evidence.

No test executable or QEMU test was run by the implementation agent, as requested.

Follow-up: the user's eight-stage run exposed a host prompt-detection race. ANSI
horizontal-scroll redraws repeat `fortress>` before Enter; the shared S6/S7
runner previously accepted any occurrence as completion. Command waits now
require the submission LF and a final prompt at the end of the checked capture.
Response extraction uses that same final boundary. Added
`scripts/test_shell_prompt_host.py` to `test-shell-host` for partial redraws,
silent commands, embedded prompt text and cwd prompts. Regression and QEMU
execution remain with the user; this fix does not establish pipeline acceptance.

## User test handoff

```sh
make test-pipeline-host
make test-shell-s7
make test-host
make test-pipe
make test-shell-s6
make test-shell-s6-resources
```

`test-pipeline-host` is also included in `test-shell-host`. It uses actual parser,
expander and executor code with mocked syscalls, asserting fd ownership before
wait, all-stage launch ordering, last-stage status, preflight rejection, scoped
arguments/environment, lexical redirections, closed standard fds, 16-action
bounds, pipe/relocation exhaustion, first/middle/last launch failures and wait
failure continuation. This is not a kernel refcount, allocator or SMP proof.

`test-shell-s7` builds a disposable ISO containing `/bin/pipetest`, a test-only
producer/relay/verifier/early-exit fixture. It uses the S6 session harness with
new optional ISO/log/audit arguments; default S6 behavior stays unchanged. Each
BIOS/UEFI run uses one CPU, paired read-only OVMF code/disposable vars and a private
NVMe fixture copy. Commands have bounded host waits and QEMU cleanup. Logs:
`build/shell-s7-{bios,uefi}.log` and matching `-e2fsck.log` files.

The runner checks 2/3/8 stages with 300,123-byte pattern comparison, grouping,
negation, preflight canaries, explicit redirect precedence, action limits,
cooperative partial-launch failure and 32 early-exit cycles. A 262,267-byte disk
payload exceeds 256 KiB while fitting the fixture's 1 KiB ext2 single-indirect
write limit; offline debugfs extraction compares every byte and `e2fsck -fn`
checks the disposable filesystem. Repetition and prompt recovery do not establish
exact kernel allocation-set equality; retain the existing pipe/resource suites.
