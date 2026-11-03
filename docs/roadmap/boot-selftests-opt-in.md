# Boot self-tests are opt-in — 2026-11-03

Normal boots initialize the OS and validate the actual platform without running
legacy stress suites. Add the exact kernel argument `boot_selftests=1` to run
those suites. Standard development media has a separate **Diagnostic Self-tests**
entry; the default and Verbose Debug entries do not enable fault injection.
QEMU automation may instead supply `opt/fortress/boot_selftests` through fw_cfg.
This flag grants no raw-storage/write permission.

## Preserved on every boot

The dependency order remains PMM low-memory initialization → VMM/CR3 → high-memory
unlock → heap → ACPI/PIC/APIC/timer → scheduler → syscall/PCI/SMP/input/network/root
initialization. Boot metadata validation, PMM audit, real boot-stack guard mapping,
initial heap integrity, ACPI checksum/MADT validation, PIC masks, LAPIC register
checks, positive timer progress verification and syscall MSR checks still run.

AP startup retains GS/GDT/TSS/stack/CR3 and IST verification as an essential
architecture contract. This includes the existing short AP IST fault probes;
the deferred BSP fault exercises and worker stress are separate. CPU-0 network
ownership, production AP scheduling, DMA quarantine and storage admission gates
are unchanged. Normal startup uses the previously established installed-root
SMP scheduler-start path, without lock-contention workers.

## Diagnostic-only work

- Deliberate BSP breakpoints, unmap/read-only/NX/stack-guard faults.
- Primitive-memory and PMM allocation/free exercises.
- Heap allocation, coalescing, injected failures and mixed-size stress.
- Synthetic MADT parser fixtures and invalid/masked timer test cases.
- Cooperative/preemptive scheduler workers and 36-thread lifecycle stress.
- User-address-space lifecycle/teardown, embedded Ring 3 and syscall exercises.
- Framebuffer console exercises, VFS/descriptor tests, PCI inventory diagnostics.
- SMP lock contention, scheduler stress, wake/IPI/TLB and timing acceptance suites.

Actual scheduler, syscall, PCI and AP initialization are outside their surrounding
legacy test routines. Explicit existing per-subsystem test keys (for example
memory pressure, pipe tests or ext4 fixtures) retain their independent gates.
Implicit QEMU device identity alone no longer selects the storage fixture suite;
its existing test keys and device/boot-mode checks remain necessary. Neither a
physical default boot nor choosing this diagnostic flag enables raw media tests.

## Verification

`make test-boot-selftests-host` tests bounded exact option parsing under ASan/UBSan,
including null/empty input, verbose-only, near matches and unterminated buffers.
`make test-boot-selftests` builds private media and tests BIOS/UEFI × SMP=1/4 ×
normal/diagnostic, with no storage devices attached. It verifies essential init
messages, absence/presence of the suites, live login, userspace execution and
shutdown. KVM is the default; `scripts/test_boot_selftests.py --accel tcg` is
available. The three legacy SMP harnesses now explicitly build text diagnostic
media and regard the login prompt as boot readiness; they do not claim interactive
shell authentication coverage (the new matrix covers login and execution).

Initial combined-workspace matrix: 8/8 PASS. For the BIOS SMP=4 case, the serial
capture contained 209 lines in normal mode versus 792 in diagnostic mode. This is
QEMU output volume, not a measured Dell boot-time improvement. Final release-source
verification and logs are under `build/boot-selftests/`; physical acceptance of the
next image remains pending. Parallel ACPI/battery edits are not part of this change.

Clean-source confirmation: isolated commit `538aa97` also passes 8/8 boots with
both normal and diagnostic login/userspace/shutdown. All four SMP=4 cases report
all four CPUs online. Evidence: `.codex-remote-attachments/boot-selftests-check/`
`build/boot-selftests-om3jxkai/manifest.json`; build/matrix logs are mirrored under
`build/boot-selftests/`. The checkout was clean during verification, excluding
parallel ACPI changes. The earlier parser-host failure was an incorrect test
capacity (the opt-in string needs 17 bytes including NUL); corrected before the
passing sanitizer run. No failed kernel boot is omitted from the matrix result.

Normal-boot BIOS/UEFI dmesg integration also passes from this clean checkout,
including explicit disposable storage-fixture admission, sudo, standalone/builtin
logging, file output, severity/tail filters and shutdown. The test's separate
`dmesg_test` sample key does not enable the extensive boot suites.
