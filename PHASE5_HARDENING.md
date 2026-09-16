# Phase 5 hardening and validation

## Completed implementation

- Kernel-owned physical RSDP snapshot for Limine base revision 3.
- Bounded firmware mappings with overflow and existing-translation checks.
- RSDP and SDT checksums, root pointer-array divisibility, unaligned XSDT
  pointer reads via memcpy, and rejection of malformed/truncated MADT records
  or supported-record capacity overflow.
- xAPIC mode and MSR/base checks, supported LVT masking, explicit hardware
  handler registration for dispatcher-owned EOI, and silent spurious counting.
- Multiple I/O APIC discovery, nonoverlapping GSI ranges, bounded selector
  indices, masked route updates, and initial mask readback. ISA routes resolve
  firmware overrides and validate polarity/trigger encodings.
- PIT channel 2 calibration with bounded polling and gate/speaker restoration.
  Calibration leaves the timer masked; the caller enables delivery last.
- Timer verification uses ten PIT 50ms intervals, not a CPU delay loop.
  A 30% tolerance allows VM scheduling variation; this is not a precision clock
  qualification. The polling iteration budget is only a timeout safeguard.

## Regression checks

Boot tests reject malformed MADT fixtures (truncated header, zero record length,
short known record, record overrun), physical address overflow, invalid timer
rates and invalid GSI/vector requests. A masked timer must fail the hardware
reference progress test. With interrupts enabled, foreground allocations and
payload checks run while ticks advance; heap integrity is checked afterward.
Timer ISRs do not allocate, print, or send their own EOI.

Validation targets: strict kernel build and packaged ISO, UEFI q35 with 2 GiB,
and BIOS q35 with 2 GiB and two advertised CPUs (only the boot CPU executes).
Serial output must reach the final Phase 5 completion message. The kernel then
halts deliberately, so the host timeout is expected.

## Boundaries and deferred verification

- External I/O APIC interrupt delivery is not yet tested. Discovery and masked
  redirection readback do not establish successful device interrupt routing.
- Drivers and handler registration are boot-CPU-only with interrupts disabled;
  SMP, runtime route changes and x2APIC are not supported here.
  Later NMI hardening adds MADT type-4 bootstrap-CPU routing and QEMU injection tests; see ARCH_REVIEW.md.
- No physical Latitude test, forced PIT hardware timeout, or arbitrary-register
  preservation stress harness was run. Existing exception tests and timer/heap
  coexistence pass, but are not substitutes for those tests.
- ACPI mappings assume firmware-supplied physical addresses describe accessible
  memory; checksums and bounds are not authentication of hostile firmware.
- Unexpected interrupts halt with diagnostics instead of silently leaving an
  in-service interrupt running. No unconditional EOI is sent for software vectors.
