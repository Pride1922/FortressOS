# Shell scripting Phases A–B — implementation and evidence

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
functions, conditions/loops, substitutions, traps, arithmetic, here-documents
and automatic shebang dispatch are not implemented. Known compound keywords
in command position are rejected before executing that command unit.
Script diagnostics currently identify input name and starting line; precise
token columns remain a follow-up. Existing shell variable/assignment rules are
reused; this delivery does not establish full POSIX assignment semantics.

The inherited `$$` behavior still expands the existing fixed value rather than
the runtime PID; do not use it for unique filenames or identity. This limitation
predates scripting and is not a concurrency primitive.

Physical console/SSH script acceptance, integration with Watchtower's updated
shell shutdown handlers, installed-root packaging acceptance, and a new USB
image remain separate. Issue #14 stays open for the later language phases.
