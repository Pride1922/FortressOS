# Shell S7 Phase 5A: stream utilities

**COMPLETE — user accepted 2026-09-27 after successful tests.** Phase 4 remains complete.
This is the utilities portion of [Phase 5](../plans/S7_PHASE5.md); Phase 5B builtin
pipeline-stage support is still open. No runtime tests were executed by the agent.

## Acceptance evidence

The user reported “tests ran perfectly” after the implementation handoff naming
`make test-stream-tools-host` and `make test-shell-s7`. Record those as user-reported
passes, not agent-executed runs. The S7 target covers BIOS/UEFI on the BSP with
disposable fixtures; the host target uses ASan/UBSan and mocked syscalls. No raw
logs or separate regression-target results were supplied in this acceptance
message. The broader handoff commands below remain regression recommendations,
not additional claimed passes. No cross-core or physical-hardware result is inferred.

Follow-up: [Phase 5B runner-path acceptance](shell-s7-phase5b.md) supersedes
the open-status statements at this historical checkpoint.

## Behavior and scope

Production initramfs includes `/bin/cat`, `/bin/head`, `/bin/tail` and `/bin/wc`.
They share a freestanding syscall/CLI/input-iteration runtime and aligned entry
stub in `user/tools/`. Read/write buffers and tail storage are static BSS. Tools
process stdin, `-` and named operands sequentially and close only owned inputs.
They retry short writes, reject zero write progress and stop quietly with 141 on
stdout EPIPE. Input failures can continue to other operands; output failures stop.

- Cat copies bytes unchanged with no added newline. The shell's former sanitized
  cat is now `view`: printable ASCII/tab/LF pass through, other bytes become dots,
  and nonempty unterminated output receives a final LF. Bare cat resolves through
  PATH; view stays a parent builtin and is rejected in pipelines. Completion uses
  the existing builtin table and filesystem discovery; no special-case mapping.
- Head defaults to ten lines, also supports byte counts, and stops when satisfied.
  Count zero performs no reads but still validates/opens named operands. Line-mode
  buffered reads can consume bytes past the emitted prefix; there is no seek-back
  on a pipe. A repeated stdin operand continues from its actual stream position.
- Tail always drains to EOF, including count zero, as its help text states.
  Byte mode retains up to 65,536 bytes; line mode has ten slots with up to 4096
  content bytes each plus LF/metadata. Excess requested counts fail before input
  I/O. An overlong line in the final selected suffix fails without emitting that
  operand; an evicted overlong line does not fail the later suffix.
- Wc uses byte-oriented ASCII separators (space/tab/LF/CR/VT/FF), counts LF bytes
  as lines, and supports selected fields and successful-operand totals. Checked
  per-file overflow suppresses the affected row; total overflow suppresses only
  the total. Both diagnose and return 1 without wrapping counts.

Each tool supports `--help` as its sole argument. Other supported option forms,
count bounds, diagnostics and statuses follow the reviewed plan. No hosted
runtime, user allocator, seek syscall, signals or scheduler changes were added.
The shell's standalone reporting now excludes 141 from the existing 128–159
fault-labelled range. That range matches the kernel's `128 + exception_vector`
convention; without termination metadata, explicit 141 and #GP remain ambiguous.
All other reporting behavior is retained. Pipe peers remain on the BSP.

## Build checks, not runtime evidence

- Built the shell, all four tool ELFs and the extended pipeline fixture with
  project freestanding `-Wall -Wextra -Werror` flags.
- Compiled the actual tools plus syscall mocks with ASan/UBSan using
  `python3 scripts/test_stream_tools_host.py --build-only build/stream_tools_host`.
  That option compiles only; it does not execute the test binary.
- Python syntax checks for the new host runner and changed integration scripts;
  reviewed compiler stack reports. The largest reported tool frame is 128 bytes;
  the deepest ordinary wc output call chain totals about 416 bytes before entry
  alignment allowance, within the existing 512-byte floor. No recursive tool paths.

## Regression commands and coverage

```sh
make
make test-stream-tools-host
make test-shell-s7
make test-host
make test-pipe
make test-shell-s6
make test-shell-s6-resources
```

The host suite uses actual utility code with mocked syscalls. Cases cover short
I/O, all byte values, repeated stdin, fd 0/1/2 ownership, missing/directory inputs,
closed stderr, EPIPE/zero-progress errors, head zero/boundaries, tail byte/line
wraparound and retained-versus-evicted overflow, ASCII word boundaries, formatting
and injected wc counter overflow. This is not scheduler or kernel refcount proof.

The existing BSP BIOS/UEFI S7 runner now exercises production tools through 2/3/8
stages, view/raw-cat differences, PATH/discovery, head/tail/wc chains and invalid
counts. It extracts `/s7copy`, `/s7samplecopy` and `/s7headbyte` from its disposable
NVMe fixture for exact byte comparison along with existing offline filesystem
checks. It retains the existing UART session/prompt handling and cleanup policy.

The test-only `/bin/pipetest observe 0/1/2` harness spawns a real cat producer and
head/tail consumer, closes all parent pipe ends, and waits for both PIDs. It
asserts producer 141 for head counts zero/one, producer 0 for draining tail count
zero, and consumer 0 in all cases. This directly observes upstream status instead
of inferring it from the shell's last-stage `$?`. The fixture stays out of the
production initramfs. Cross-core and hardware acceptance remain Phase 6 work.
