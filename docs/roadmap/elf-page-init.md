# ELF page initialization experiment — 2026-10-09

The Dell kernel stack ABBA result supports retaining batch mapping (17.6%
lower plain spawn duration). The next experiment isolates ELF page contents.
Earlier Dell attribution placed approximately 7.3 ms of 25-spawn ELF work in
copy/zero and 4.0 ms in mapping, inclusive of scheduling and contention.
The build's scalar memset/memcpy loops run without optimization; each file
page was fully cleared before file bytes overwrote the same locations.

`elf_page_init` uses integer `rep movsb`/`rep stosb`, with explicit forward DF,
proper register constraints and a memory clobber. It copies the validated
file slice once and zeroes the page prefix and suffix. BSS-only pages and
the user stack remain entirely zero; the restorer is copied with zero tail.
No SIMD state, permission, ELF acceptance, page mapping, CR3 ownership or
publication contract changes. Frames remain private/unmapped until complete.
The ordinary kernel string routines are unchanged. Mapping is deliberately
left for a separate experiment so effects can be attributed.

## Validation

- `make test-elf-page-host`: ASan/UBSan, 16,388 cases covering every page
  offset, zero/full/partial slices, unaligned source/destination and surrounding
  canaries; exact expected bytes throughout each page.
- Actual `elf.c` host fixture: freshly poisoned frames, unaligned multi-page
  RX segment, full RW/NX page, BSS, restorer and zero stack; every byte and
  leaf permission checked. Allocation failure at all seven reservations and
  mapping failure at all six leaves reclaim everything and leave output zero.
  W^X and truncated file are rejected before allocation. Host mocks do not
  establish hardware mapping behavior.
- Strict freestanding build PASS.
- BIOS/UEFI × 1/4/8 CPU ISO-only lifecycle suite: 6/6 PASS, 100 spawn/exit
  cycles, exact physical allocation set/table equality and zero deferred
  destruction. Logs `build/smp-vmm-lifecycle-{bios,uefi}-{1,4,8}-2G.log`.
- Candidate BIOS and UEFI SMP=8: spawn_wait, signals and pipes, warmup plus
  two timed profiled repetitions each; barrier/phase accounting PASS, panic
  capture armed, no panic observed. Evidence `build/elf-copy-{bios,uefi}-20261009`.
- Control A BIOS SMP=8: the same three profiled workloads, warmup plus one
  timed repetition, pass; `build/elf-copy-control-A-20261009`.
- Both A/B UEFI USB SMP=1 disposable copies pass profiled signals/pipes,
  persist a >4096-byte `/mnt` capture independently validated with debugfs,
  cleanly shut down and pass offline `e2fsck -fn`. Frozen source hashes stay
  unchanged; `build/elf-copy-usb-{A,B}-20261009`. No physical or SMP=8 USB
  claim follows from this check.

## Matched hardware comparison

`build/elf-copy-ab-20261009` freezes A scalar clear-then-copy versus B integer
string copy-and-clear-complement. Both use the proven batch kernel stack
mapper. All other linked object bytes and benchmark/initramfs are shared.
The packager relinks isolated objects, checks B matches the workspace ELF,
and verifies exact kernel/initramfs bytes embedded in both raw and ISO images.
Sources, helper header, object snapshots, command templates and hashes are
retained. `make prepare-elf-copy-comparison` creates another fresh bundle.

| Image | SHA-256 |
| --- | --- |
| A | `73b2ac7baa0d31b241d35336b600401152ab7c1a885328d3a9b3811cb4ad8271` |
| B | `ffc22dad67a8d3bcdcc707adaafdd591b1920c038aa97368314d8413a401d51e` |

Run A1 -> B1 -> B2 -> A2 using `commands.txt`, Persistent Storage RW and the
same power/firmware/background/thermal conditions. Export every shutdown to
`build/elf-copy-dell-results/{A1,B1,B2,A2}` before overwriting USB logs or
reflashing. B2 reboots the same B stick. Full stdout goes to `/mnt`; do not
overwrite the before lock snapshot after benchmarks. Compare plain medians
and retain every repetition; use profile copy/map phases for attribution.
No Dell performance gain is claimed until this comparison is collected.

## Dell ABBA results

The user exported A1/B1/B2/A2 to `build/elf-copy-dell-results`.
Fifteen complete workload logs validate: eight online CPUs, expected common
benchmark hash, full readiness/worker records, `ok=1`, `short=0`; seven
profile logs additionally pass phase/spawn/writer accounting. All 48 plain
and 35 profiled timed cohorts are retained, excluding warmups. A2's
`elf-copy-13-pipes-profile.txt` is zero bytes and is explicitly excluded;
its absence is not treated as a workload failure or a passing result.
Image variant identification relies on user boot/export labels, not the
common benchmark hash. Raw source-file hashes are retained with analysis.

Plain median duration, milliseconds (lower is better):

| Workload | A1 | B1 | B2 | A2 |
| --- | ---: | ---: | ---: | ---: |
| spawn_wait | 30.071 | 14.616 | 14.558 | 30.104 |
| pipes | 15.389 | 18.824 | 18.679 | 15.186 |

The pooled plain spawn median falls 30.0875 to 14.5735 ms: **51.6% lower
duration**, approximately 2.06 times throughput for fixed work. Both B boot
medians beat both A medians. Profiled spawn medians similarly fall 30.143
to 14.7425 ms (51.1%). This is a strong spawn result for the tested hardware
and eight-worker configuration, not a general scaling claim.

Mean attribution to the slowest worker in each profiled spawn cohort
(ten cohorts per variant, inclusive elapsed milliseconds per 25 spawns):

| Interval | A | B |
| --- | ---: | ---: |
| ELF copy/zero | 7.543 | 0.141 |
| ELF mapping | 4.018 | 4.672 |
| Whole ELF loader | 12.435 | 5.662 |
| Kernel stack allocation | 2.909 | 1.368 |
| Worker spawn phase | 17.813 | 8.631 |
| Worker wait phase | 12.166 | 6.080 |

Copy/zero falls **98.1%**. Mapping increases about 16.3% and now dominates
ELF work; it remains a useful next spawn optimization target. The unchanged
stack mapper and wait phase also become faster, so whole-worker benefit
cannot be described as a direct subtraction of copy CPU time alone.
These intervals include scheduling/contention, and the selected critical
worker can change across cohorts.

Pipes shows an adverse plain result: pooled median rises 15.3535 to 18.750
ms (**22.1% slower**), with both B boots slower than both A boots. Profiling
reverses the direction: available medians are A1 19.540, B1 15.151, B2
15.282 ms; A2 is missing. The profiled subset cannot override the primary
plain regression. Treat B as a confirmed spawn optimization with an
unresolved pipe timing regression, not an unconditional workload win.
Additional plain interleaved trials and reader wake-to-selection attribution
are appropriate before declaring this tradeoff acceptable. Recover/re-export
the empty A2 profile if its original USB log still exists.

Aggregate lock/TLB intervals are not matched workload counts: B2 records
13,374 remote batches versus A1 9,576/B1 9,571, while A2 records 9,225 and
lacks its final profile capture. Extra activity/retries or capture differences
cannot be identified from these snapshots. Do not pool these counters into
a causal per-workload comparison or subtract cumulative max-spin values.

Reproducible analysis: `build/analyze_elf_copy_dell.py`,
`build/elf-copy-dell-validation.json`, and
`build/elf-copy-dell-profile-analysis.json`. No kernel changes were made
while analyzing these hardware results.
