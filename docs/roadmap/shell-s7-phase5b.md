# Shell S7 Phase 5B: builtin pipeline stages

**COMPLETE — user-confirmed 2026-09-27.** S7 Phases 1–6 are now complete.
The subsequent [Phase 6 acceptance](shell-s7-phase6.md) covers QEMU BIOS/UEFI
with 1/4/8 CPUs and Dell Latitude 5590 in RO and RW mount modes. Pipe peers
remain BSP-pinned; cross-core execution is deferred. Design references:
[S7 plan](../plans/S7_PLAN.md) and [Phase 5B plan](../plans/S7_PHASE5B.md).

## Delivered scope

`/bin/sh-builtin` is a standalone ELF spawned through `SYS_SPAWN_EXT` for each
builtin pipeline stage. It dispatches one allowed command and exits. The bounded
allowlist is `echo`, `pwd`, `true`, `false`, `env`, `help`, `version`, `ls`, `view`
and `type`. Other builtins are rejected during group preflight, before pipes,
children or redirection opens. Direct `/bin/sh-builtin NAME [ARG...]` invocation
uses the same allowlist.

Implementation references are `user/shell/pipeline.c`, `user/sh_builtin_main.c`
and shared handlers in `user/shell/builtin_exec.c/.h`. Both parent and runner
link the shared handlers. The Makefile's `SH_BUILTIN_OBJECTS` and runner link
rule restrict the ELF to its entry/dispatcher, `builtin_exec.c`, `builtins.c`
and syscall/status-bearing I/O helpers from `io.c`; editor, history, prompt and
terminal UI are outside that link boundary.

## User-process entry envp finding

The runner faulted while dereferencing envp in Ring 3: page fault error code
`0x5`, CR2 `0xffffffff80062bb8`. Frame construction in `src/kernel/thread.c`
correctly stored the user envp table address in saved RBP (`frame[4] = rdx_val`).
The trampoline transferred R14 to RDI and R15 to RSI for argc/argv, but omitted
the RBP-to-RDX transfer.

RDX consequently retained a caller-clobbered value from `sched_post_switch`.
In the inspected kernel, its zombie-thread check loads `scheduler_cpus + 0x1578`,
resolving to `0xffffffff80062bb8`, into RDX. This was a legitimate kernel data
address left in a scratch register, not evidence that saved RBP was corrupted.
The address is kernel image data, not an HHDM translation.

`src/arch/x86_64/context.asm` now executes `mov rdx, rbp` beside the argc/argv
transfers, before clearing RBP. The debugging `push rbp` / `pop rbp` pair was
removed: RBP is callee-preserved, and the single push misaligned the otherwise
16-byte-aligned stack before the C call. `sched_post_switch`, context-frame
layout, argv transport, address-space cleanup and user code were unchanged by
this fix. Scalar kernel spawns still supply zero through saved RBP.

Disassembly evidence: `objdump -d --disassemble=user_process_trampoline
bin/fortress.elf` confirmed the aligned direct C call and transfer before
register clearing. The corresponding `sched_post_switch` disassembly identified
the exact residual address. The original focused investigation is preserved in
[user-entry-envp.md](user-entry-envp.md).

## Runner assertions and reporting correction

`scripts/test_shell_s7.py` now asserts exact `PATH=/bin`, `HOME=/` and exported
`PIPE_TEST=s7b` lines for both `/bin/sh-builtin env` and `env | cat`, alongside
the existing `echo hello | cat` assertion. The malformed negation case now
passes expected status `1` as a Python argument instead of appending `, 1` to
the shell command.

The utility loop shadowed the firmware variable `mode`, causing both completion
messages to print `2`. Renaming that loop variable to `stream_mode` was a
reporting-only correction, initially syntax-checked without a QEMU rerun. Later
user-reported suite passes are recorded in Phase 6. During the
passing run, each audit was copied immediately after its firmware run to
`build/shell-s7-{bios,uefi}-e2fsck.log`, preserving both before overwrite.

## Verification evidence

The disassembly, NMI and S7 results below were obtained during the preceding
envp-fix session. The pipe and S6 regression passes were supplied by the user
with this acceptance record. No tests were rerun for this documentation update.

| Command | Result and evidence boundary |
| --- | --- |
| `make test-nmi` | PASS, QEMU BIOS and UEFI: 28 exact-boundary NMIs per firmware plus each complete boot suite; disposable/snapshot NVMe fixture. Evidence: `build/nmi-*.json` and `.log`. |
| `make test-shell-s7` | PASS, QEMU BIOS and UEFI, one CPU, disposable ISO and NVMe copy: exact environment entries, echo pipeline, existing streaming/status checks, extracted-file byte comparisons and clean `e2fsck -fn`. Serial logs: `build/shell-s7-bios.log` and `build/shell-s7-uefi.log`; firmware-labelled audit logs as described above. |
| `make test-pipe` | User-reported PASS, QEMU BIOS and UEFI, one CPU, no data disks. |
| `make test-shell-s6` | User-reported PASS, QEMU BIOS and UEFI, Phase 4 and Phase C integration checks. |
| `make test-shell-s6-resources` | User-reported PASS, QEMU BIOS and UEFI, 1 and 4 CPUs: 12 measured cycles per cell, exact cleanup, refcount balance, slot reuse and clean `e2fsck`. This is resource regression evidence, not cross-core pipe execution. |

No new host sanitizer execution is claimed for this checkpoint. Its original
runtime evidence was QEMU BSP evidence. Subsequent QEMU 1/4/8-CPU and physical
Dell RO/RW acceptance is recorded separately in [Phase 6](shell-s7-phase6.md),
so the hardware result is not attributed to the earlier envp-fix session.
`AGENTS.md` now records S7 completion; the plan's original cross-core G6 wording
is qualified by the accepted BSP-pinned scope in Phase 6.
