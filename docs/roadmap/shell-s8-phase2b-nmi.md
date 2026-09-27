# Shell S8 Phase 2B — sigreturn NMI gate implementation

2026-09-27: implementation handoff; QEMU acceptance is **pending**. No new
BIOS/UEFI or hardware pass is claimed here.

Run `make test-nmi` in WSL Ubuntu-24.04. It builds the dedicated Ring 3 fixture
and runs the original seven-boundary SYSRET suite, followed by a separate
sigreturn fixture boot, under each of BIOS and UEFI. The production initramfs
is unchanged. Each run uses the existing snapshot NVMe fixture; UEFI uses
read-only OVMF code and disposable paired variables.

The four new assembly labels emit no bytes. `sigreturn_after_swapgs` and
`sigreturn_before_iretq` alias the same IRETQ instruction. Per firmware, the
expected coverage is 28 original NMIs plus 24 sigreturn NMIs (three distinct
addresses, two origins, four rounds). The aliased label also receives four
roundtrips per origin but contributes no additional NMI count. Reports enforce
coverage per origin/address, rather than relying on the aggregate count.

The fixture installs a SIGTERM handler and uses self `kill` for syscall-origin
delivery. A separate child announces readiness, then executes a syscall-free
assembly loop with distinct values in every GPR and DF set. Its parent sends
SIGTERM after a bounded timed input wait. The runner requires the saved vector
to be the LAPIC timer vector (32), and the saved RIP to be in that loop; an early
delivery at the ready syscall cannot pass. The handler returns through the
kernel-mapped restorer, whose address, bytes and entry RSP are checked.

The debugger captures the kernel context before signal-frame construction and
compares it with the user signal frame, committed sigreturn frame, and actual
Ring 3 registers immediately after IRETQ. Checks cover all GPRs (including
RAX/RCX/R11), permitted RFLAGS including DF, RIP/RSP, selectors, IST2 entry,
exact NMI interruption address, actual GS base from QEMU, unchanged kernel
frame/stack and user-stack bytes, and unchanged syscall scratch/RSP0 fields.
Only hardware breakpoints and QMP `inject-nmi` trigger the injections; no guest
memory writes, INT 2, or transition-window waits are introduced. A separate
Ring 0 recovery check observes the legacy INT 0x80 test: it captures the target
and stack passed to `syscall_set_recovery`, then matches that target at
`isr_return_iretq` and verifies kernel GS and its IRET result. This is not a
claim of fast-syscall recovery-path execution. The fixture must
also finish, reap each timer child successfully, and print its PASS marker.

Evidence files after successful execution:

- Original suite: `build/nmi-{bios,uefi}.{log,json}`.
- New suite: `build/nmi-sigreturn-{bios,uefi}.{log,json}`; JSON separates kernel
  test recovery from boundary results and records aliases/non-injected cases.

## Existing implementation discrepancy

The checked-in dispatcher uses a kernel-written vector marker (`0x100`) rather
than the separate 16-byte disposition scratch slot specified in S8_PLAN.md.
This handoff preserves that mechanism. Assembly now selects the sigreturn
branch before any GPR pop and uses a separate identical pop sequence, exposing
the required first-pop boundary. There is no scratch area to remove in this
implementation. The ordinary SYSRET sequence, its seven probe labels, and the
kernel recovery return retain their existing instructions. This gate does not
claim implementation of the plan's separate disposition-slot design.

## Checks performed for this handoff

- `make build/s8_nmi_user.elf build/arch/x86_64/syscall_entry.o`: compiled.
- Python AST parsing of the runner: passed.
- `nm -n build/arch/x86_64/syscall_entry.o`: confirmed all four global labels
  and the after-SWAPGS/before-IRETQ alias.
- No runtime tests run; requested test execution remains with the user.

## Recovery runner correction

The user's initial BIOS run passed the original 28-NMI suite, then timed out
waiting for `syscall_return_iretq`. Its serial log showed repeated fixture PASS
markers: the guest continued while that unreachable breakpoint remained armed.
The boot's direct recovery fixture uses INT 0x80; scheduled fast-syscall process
exits do not visit the fast entry stub's kernel recovery branch. The runner now
matches the explicitly armed legacy recovery target at the common ISR IRET,
with a bounded search. Sigreturn boundary and origin requirements are unchanged.
Runtime verification of this correction remains pending.

The user's next BIOS run passed the separate recovery check, then failed the
runner's incorrect nonzero-frame-ID assertion. `process_signal_push_frame`
assigns the zero-initialized generation counter before incrementing it, making
ID 0 valid. The runner now checks the exact expected ID (0 through 15 in the
persistent parent, 0 in each newly spawned timer child) and records it in JSON.
Header checks now report version/size, generation and reserved-field failures
separately. Runtime verification of this correction remains pending.
