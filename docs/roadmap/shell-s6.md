# Shell S6 — Phase 4 and Phase B formal validation

2026-09-26: Phase 4 and Phase B are formally closed across a fresh five-gate evidence
set executed on the current tree. Full milestone acceptance remains open for the
outstanding checks in [S6_AUDIT.md](../plans/S6_AUDIT.md).

`/bin/dual_stream` checks entry stack alignment, writes twelve bytes to each
standard output stream and tolerates EBADF on closed stderr. It is packaged in
the initramfs. The S6 runner verifies lexical duplication ordering, repeated
stderr append, stderr truncation, closed stderr with exit status 0, redirected
stdin through `cat`, and parent setup failure with closed stderr and status 1.
File contents are compared as complete normalized UART payloads after removing
command echo and prompt, not as raw disk bytes.

The shell I/O helper returns completed byte counts or negative errors,
retries positive short writes, and fails on zero progress without recursive
diagnostics. Host tests exercise the actual helper with mocked syscall results.
Spawn failure diagnostics use stderr. `cat` without operands reads stdin and
propagates output failures to command status. Legacy void output wrappers still
discard write results; universal builtin write-error status handling is not
claimed. `n<&m` is host-verified through the parser and action builder.

The Phase B resource suite (`tests/s6_resources_user.c`, `scripts/test_shell_s6_resources.py`)
verifies descriptor and process table limits across a 4-run matrix (BIOS and UEFI,
1 and 4 CPUs). A GDB breakpoint harness verifies that aborted spawns clean up all
descriptors, release address spaces and TCBs, and leave 0 runnable or zombie child
threads in kernel queues prior to publication.

The following five gates ran in order, each with exit status 0:

1. `wsl -d Ubuntu-24.04 -- make test-shell-host`: ASan/UBSan PASS.
2. `wsl -d Ubuntu-24.04 -- make test-shell-s6`: BIOS and UEFI PASS (all 11 Phase 4A–4D
   checks and checklist item 4 dual-stream/redirection cases).
3. `wsl -d Ubuntu-24.04 -- make test-shell`: BIOS/UEFI, UEFI 8 GiB no-COM1,
   and S3–S4 PASS, including existing spawn/restart resource checks.
4. `wsl -d Ubuntu-24.04 -- make test-smp-append`: BIOS/UEFI with four CPUs
   PASS. Independent and shared append each retained 200 records without loss or
   duplication. Interleaving transitions were 102/110 on BIOS and 117/110 on UEFI
   (varied from prior 53/60 and 67/61 runs due to host scheduling/timing; correctness
   invariants remained identical). Clean S5 shutdown and offline `e2fsck -fn` passed.
5. `wsl -d Ubuntu-24.04 -- make test-shell-s6-resources`: BIOS/UEFI with 1 and 4 CPUs
   PASS. Child descriptor limit (32), parent fd exhaustion (31 fds, `SYSCALL_EMFILE`),
   process table capacity (`SYSCALL_ENOMEM`), 0 partial-child thread leaks, and prompt recovery.

Evidence: `build/shell-s6-{bios,uefi}.log`, `build/shell-{bios,uefi}-1cpu.log`,
`build/smp_append_{bios,uefi}_4.log`, `build/shell-keyboard-only.png`, and
`build/shell-s6-resources-{bios,uefi}-{1,4}cpu.log`. Storage tests used disposable
NVMe fixtures.

### Remaining for S6 Milestone

- **Finding 8:** Trace half closed via Phase B scheduler-ref inspection / breakpoints;
  shared-offset rule documentation decision remaining for concurrent non-append `file_t`.
- **Phase C:** RO/tainted storage assertions (next).
- **Phase D:** Dell hardware checklist (item 6) on explicitly selected writable USB (blocking).
- **Item 7:** Mark S6 complete (gated on Phase C, Phase D, and Finding 8).
- Pipelines belong to S7.
