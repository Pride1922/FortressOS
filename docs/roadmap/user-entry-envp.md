# User-process entry envp fix (2026-09-27)

The working tree's process frame already put the user envp table address in
saved RBP, but `user_process_trampoline` did not copy RBP to RDX. RDX therefore
retained a caller-clobbered value from `sched_post_switch`. In the inspected
kernel, its zombie-thread check loads `scheduler_cpus + 0x1578`, exactly
`0xffffffff80062bb8`, into RDX. This explains the reported Ring 3 fault without
RBP corruption. The address belongs to kernel image data; HHDM translation is
not involved.

The trampoline now executes `mov rdx, rbp` alongside the existing argc/argv
transfers, before clearing RBP. The diagnostic push/pop of RBP was removed:
RBP is already callee-preserved, and a single push misaligned the otherwise
16-byte-aligned stack before the C call. `sched_post_switch`, context-frame
layout, argv transport, address-space cleanup and user code remain unchanged.
Scalar kernel spawns continue to supply zero through their saved RBP slot.

The S7 runner now checks exact PATH, HOME and exported PIPE_TEST entries from
both direct `/bin/sh-builtin env` and `env | cat`, alongside its existing echo
pipeline. Its malformed negation test was corrected to pass expected status 1
as a Python argument instead of including `, 1` in the shell command.

Verification is scoped to QEMU BSP and disposable fixtures, not hardware or
cross-core pipe acceptance. The broader Phase 5B milestone is not declared
complete by this focused fix.

Subsequent user acceptance of the runner path and additional regression results
are recorded in [S7 Phase 5B](shell-s7-phase5b.md). The scope above describes
this earlier focused fix, not that later acceptance.

- `objdump -d --disassemble=user_process_trampoline bin/fortress.elf`: confirmed
  aligned direct C call and RBP-to-RDX transfer before register clearing.
- `make test-nmi`: passed BIOS and UEFI, 28 exact-boundary NMIs per firmware
  plus each complete boot suite (disposable/snapshot NVMe fixture).
- `make test-shell-s7`: passed BIOS and UEFI, one CPU, disposable ISO and NVMe
  copy; exact environment-entry assertions, echo pipeline, existing streaming
  and status checks, extracted-file byte comparisons and clean `e2fsck -fn`.
  Serial logs: `build/shell-s7-bios.log` and `build/shell-s7-uefi.log`.
  This run exposed an existing utility-loop variable shadowing `mode`, so both
  completion messages printed `2`. Each audit was copied immediately after its
  firmware run to `build/shell-s7-{bios,uefi}-e2fsck.log` before the next could
  overwrite it. The loop variable was renamed to `stream_mode`; that reporting-only
  correction was syntax-checked, not rerun through QEMU.
