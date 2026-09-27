# Shell S7 Phase 6: QEMU and physical acceptance

**COMPLETE — user-confirmed 2026-09-27.** Shell S7 Phases 1–6 are complete.
Next: Shell S8 (jobs, signals and process groups). Prior implementation evidence:
[Phase 5A](shell-s7-phase5a.md) and [Phase 5B](shell-s7-phase5b.md).
Design references: [S7 plan](../plans/S7_PLAN.md) and its
[G6/G7 matrix](../plans/S7_PLAN.md#5-acceptance-audit-checklist--verification-matrix).

## QEMU acceptance

The user reported successful S7 verification under BIOS and UEFI with one,
four and eight CPUs, with AP counts confirmed in boot logs. These are
user-reported acceptance results; no QEMU tests were rerun for this documentation
update. Earlier agent-executed one-CPU results remain recorded in Phase 5B.

| Invocation | BIOS | UEFI | Boot evidence |
| --- | --- | --- | --- |
| `make test-shell-s7 SMP=1` | PASS | PASS | One CPU; no APs |
| `make test-shell-s7 SMP=4` | PASS | PASS | Four CPUs; three APs confirmed |
| `make test-shell-s7 SMP=8` | PASS | PASS | Eight CPUs; seven APs confirmed |

`SMP=N` plumbing is present in `scripts/test_shell_s7.py`: the runner reads the
`SMP` environment variable (default `1`) and passes it to the shared
`qemu_session(..., smp=SMP)` harness in `scripts/test_shell_s6.py`, which builds
QEMU's `-smp` argument. The completion message includes the CPU count.
Command-line make assignments propagate to the recipe environment.

The suite uses a disposable ISO and NVMe copy, covering streaming through
2-, 3- and 8-stage pipelines, payloads exceeding 256 KiB, stream utilities,
builtin stages, statuses and cooperative cleanup. Existing extracted-file byte
comparisons and offline `e2fsck -fn` checks remain part of the suite. Logs are
`build/shell-s7-{bios,uefi}.log` and matching `-e2fsck.log` files; filenames do
not include CPU count and subsequent runs overwrite them. The six-cell result
and AP-count confirmation above are the user's report, not a claim that six
separate log sets were inspected or archived in this documentation update.
No numerical throughput or contention measurements were supplied here.

## Dell Latitude 5590 acceptance

The user confirmed physical pipeline and stream-utility acceptance in both
read-only and writable mount modes. Reported working hardware pipelines include:

```sh
cat file | head -n 5 | wc -l
echo hello | wc -l
env | cat
pwd | cat
ls /bin | head -n 5
```

Both RO and RW passes are recorded as user-reported hardware evidence, separate
from QEMU and host checks. No new host sanitizer result, raw hardware capture,
throughput figure or per-command transcript is inferred from this acceptance.
The envp entry fix enabling the runner is documented in
[Phase 5B](shell-s7-phase5b.md): the omitted `mov rdx, rbp` left the exact kernel
address `0xffffffff80062bb8 = scheduler_cpus + 0x1578` in RDX.

## Accepted scope and remaining boundary

Phase 6 completes verification of pipelines when other CPUs exist and on the
Dell. It adds test CPU-count plumbing, not a new pipe or scheduler architecture.
Pipe peers remain pinned to the BSP, so all pipeline stages run on CPU 0 even
in a four- or eight-CPU VM.

The original plan's G6 requests a producer on CPU 1 and consumer on CPU 2.
That arrangement, and CPU 1 producer / CPU 0 consumer, remain outside this
accepted BSP-pinned scope. The literal cross-core G6 criterion is deferred to
a future milestone; the reported SMP passes do not establish distributed pipe
execution or cross-core wakeup safety. Earlier roadmap handoffs assigning that
work to Phase 6 are superseded by this accepted scope. G7's Dell acceptance is
user-confirmed in both mount modes. S7 is complete within these boundaries.
