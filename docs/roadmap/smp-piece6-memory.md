# SMP Piece 6 — Memory safety implementation and evidence

Status (2026-09-24): **6A implemented; user reports host ASan/UBSan and
2 GiB QEMU matrix and BIOS/UEFI 8 GiB/8 CPU cases PASS.
Dell photo confirms both 6A cleanup checks and all five high-memory probes.
Other RAM configurations and remaining integration checks are not yet reported.**
6B–6D remain planned. This is not acceptance of Piece 6.
Approved design: [implementation plan](smp-piece6-plan.md).

## 6A: Boot memory readiness

- All PMM allocation paths and the bitmap's entire backing are restricted to
  physical addresses below 1 GiB before VMM readiness. Fully contained usable
  pages alone enter the free bitmap; overflowing ranges are excluded.
- VMM constructs RAM mappings with bounded range arithmetic, activates the
  kernel CR3 and reads it back before publishing readiness. PMM verifies that
  publication and the active root before lifting its ceiling under rank 4.
- SMP refuses AP release without readiness; each AP checks readiness and its
  active kernel CR3 before publishing online. AP bring-up remains serialized.
- Managed free pages include high RAM even during boot. The new
  `pmm_get_allocatable_pages()` metric reports only currently eligible free
  pages, so capped OOM cannot be mistaken for exhausted managed RAM.
- Bootloader reclamation is explicitly disabled (returns zero). No callers
  used it previously. Limine handoff/module lifetime must be proven separately.
- No shootdown protocol, context-switch ABI or lifetime registry change in 6A.
  The later `vmm_space_t` kernel record will be static, as the approved plan
  specifies. Existing Phase 5 contention/timeout limitations remain for 6C.

## Verification handoff — user executes

Run in WSL Ubuntu-24.04 at `/mnt/c/Sources/FortressOS`:

```bash
make
make test-pmm-boot-host
make test-smp-memory-boot
python3 scripts/test_smp_memory_boot.py --ram 256M --cpus 1
python3 scripts/test_smp_memory_boot.py --ram 8G --cpus 8
# Optional larger QEMU case; requires sufficient host memory:
python3 scripts/test_smp_memory_boot.py --ram 32G --cpus 8
make test-smp-percpu
make test-smp-ipi
```

The host harness compiles actual PMM code with single-threaded lock and
readiness/CR3 shims under host ASan/UBSan. It checks low-memory exhaustion
despite free high RAM, contiguous allocation crossing the ceiling, premature
unlock rejection, alignment, partial usable-page exclusion, exact allocation
set recovery and bounded test-mode parsing. It does not prove SMP exclusion
or hardware CR3 behavior. Concurrent stress/TSan belongs to 6B.

The QEMU runner defaults to BIOS and UEFI, 1/4/8 CPUs, 2 GiB. It creates a
temporary ISO with `smp_memory_test=boot`; normal `limine.conf` and build ISOs
are unchanged. No data disks are attached. UEFI uses read-only paired OVMF
code and a disposable vars copy. Each boot has a 300-second deadline and
bounded terminate/kill cleanup. Logs are `build/smp-memory-boot-*.log` and
`.stderr`; no prior-run log can satisfy a new run.

Required evidence: early allocator assertions and exact cleanup; kernel CR3
and unlock ordering; full-page HHDM probes at every applicable threshold;
readiness before AP release; all requested APs online; shell prompt.
Unsupported RAM thresholds explicitly skip. On a 32 GiB run, all of 1, 2, 4,
16 and 30 GiB thresholds must pass. Actual allocated addresses may exceed the
threshold when a page is reserved. Every page is checked word by word using
an address-derived pattern and its complement, then returned to the allocator.
Static snapshot storage adds 2 MiB of kernel BSS, retained in each baseline.

For Dell 6A evidence, add `smp_memory_test=boot` to the selected Limine entry's
kernel arguments, preserving its existing PARTUUID/mode arguments. On 8 CPUs
and 32 GiB, require all five high probes, both cleanup markers, all seven APs
online and interactive shell/storage behavior. Save the log using the existing
explicit USB policy. These checks do not claim later concurrent memory stress.

## Results

| Check | Result |
| --- | --- |
| Kernel build | Bootable image exercised by user; standalone build output not supplied |
| Host ASan/UBSan | PASS, user rerun after header-search fix in 72ea691; capped exhaustion, contiguous boundary, unlock gates, rounding, exact allocation set and cmdline |
| BIOS/UEFI 2 GiB, 1/4/8 CPUs | PASS, user-supplied runner output (see below) |
| BIOS/UEFI 8 GiB, 8 CPUs | PASS, user-supplied runner output; command below |
| Other RAM configurations and regressions | Pending user execution |
| Dell 5590 memory checks | PASS: supplied photo shows early ceiling/unlock rejection, kernel CR3 activation, all five full-page readbacks and post-unlock exact cleanup |
| Dell later integration | Earlier Piece 5 excerpt reports all IPI checks PASS with 7 AP acknowledgments; same-boot shell/storage and AP-readiness output not shown in memory photo |

User-reported `make test-smp-memory-boot` results for the 6A implementation:

| Firmware | CPUs | RAM | Elapsed | Result |
| --- | --- | --- | --- | --- |
| BIOS | 1 | 2 GiB | 9.4 s | PASS |
| BIOS | 4 | 2 GiB | 13.8 s | PASS |
| BIOS | 8 | 2 GiB | 7.2 s | PASS |
| UEFI | 1 | 2 GiB | 20.1 s | PASS |
| UEFI | 4 | 2 GiB | 8.8 s | PASS |
| UEFI | 8 | 2 GiB | 15.2 s | PASS |
| BIOS | 8 | 8 GiB | 14.4 s | PASS |
| UEFI | 8 | 8 GiB | 10.6 s | PASS |

The 8 GiB cases were reported after
`wsl -d Ubuntu-24.04 -- python3 scripts/test_smp_memory_boot.py --ram 8G --cpus 8`.
The runner requires high-memory readback thresholds at 1, 2 and 4 GiB for
this configuration; it does not cover 16 or 30 GiB.

Dell excerpt supplied alongside those results: unicast delivery, broadcast
shootdown (7 AP acknowledgments), remote wake and VMM unmap/barrier all PASS,
ending with Piece 5 completion. This is additional physical Piece 5 regression
evidence. A subsequent photo supplies the earlier memory checks below.

### Dell 6A photo evidence

User-supplied photo `codex-clipboard-67399e46-184a-4d99-b6b0-5590241bbca6.png`
(reported Dell bare-metal run) visibly records:

- PASS: boot ceiling, early unlock rejection, exact cleanup.
- Kernel CR3 switch to physical root `0x2000` survived; high-memory allocation
  unlocked afterward in the log.
- PASS: full-page HHDM readbacks at the following minimum/actual addresses:

| Threshold | Minimum physical address | Allocated physical address |
| --- | --- | --- |
| 1 GiB | `0x40000000` | `0x40000000` |
| 2 GiB | `0x80000000` | `0x80000000` |
| 4 GiB | `0x100000000` | `0x100000000` |
| 16 GiB | `0x400000000` | `0x400000000` |
| 30 GiB | `0x780000000` | `0x780000000` |

- PASS: kernel CR3, high-memory unlock, exact cleanup.
- Execution continued through the breakpoint recovery check into VMM tests.

This confirms the photographed 6A boot-memory assertions. The command line
is cropped; test execution itself establishes that the mode was enabled.
The photo does not show later AP readiness, shell/storage operation or the
unreported regression suites, and does not establish completion of Piece 6.

Host evidence: user ran `wsl -d Ubuntu-24.04 -- make test-pmm-boot-host`
and supplied this successful result:

```text
PASS PMM boot: capped exhaustion, contiguous boundary, unlock gates, rounding, exact allocation set, cmdline
```

The initial compilation failed because `-Isrc/include` shadowed the hosted
`<string.h>` with the kernel header. The runner now uses `-iquote` for project
and shim headers; the successful rerun supersedes that compilation failure.

Sources: user-pasted terminal summaries and the reviewed Dell photo above;
complete raw serial logs were not independently reviewed. QEMU evidence is
limited to the reported 2/8 GiB cases; the Dell photo separately establishes
the 16/30 GiB readbacks. No agent-run tests.

The agent writes code and tooling; the user runs them. Both review supplied
evidence before recording acceptance. Historical Phase 9H hardware evidence
is preserved, but its documented cap/unlock was missing from the Phase 5
checkout. This change supplies that invariant; the available history did not
establish how the discrepancy arose.
