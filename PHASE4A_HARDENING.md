# Phase 4A hardening

## Implemented

- Required boot mappings fail explicitly on error; oversized boot memory maps
  halt instead of truncating kernel-owned metadata.
- HHDM construction skips reserved holes and maps the framebuffer explicitly.
- Boot and IST1 stacks have unmapped guards. IST1 uses a single page-aligned
  structure with compile-time adjacency checks and pre-CR3 mapping checks.
- Existing intermediate tables propagate user permissions. All four mapping
  and query operations reject noncanonical virtual addresses.
- The NX probe uses an assembly helper with aligned calls and balanced stack
  recovery. Boot tests cover both the NX fault and executable RET paths.
- Framebuffer rendering uses kernel-owned boot metadata.

## Verification

Build with `make -B bin/fortress.elf` and package with `make iso`. Boot the ISO
in QEMU and require the final Phase 4A completion message with no FAIL/FATAL
messages. Check NX fault code 0x11, executable control success, real boot-guard
fault, and rejection of a noncanonical alias of a mapped page.

Compile gdt.c with optimization as well as the default flags to verify that
stack adjacency no longer depends on global-variable ordering.

### Verified on 2026-09-15

- Strict kernel rebuild and ISO packaging passed.
- QEMU q35 with 2 GiB RAM completed all boot tests in BIOS and UEFI modes,
  with no FAIL/FATAL/PANIC messages. The NX fault reported 0x11; the executable
  control returned normally; the real boot guard fault reported 0x2.
- All four VMM operations rejected the noncanonical alias test.
- gdt.c compiled with `-O2 -Wall -Wextra -Werror` and the layout assertions.
- Assembly inspection confirmed aligned helper calls and removal of the NX
  target return address before the recovery path calls C.

For split OVMF firmware, use paired pflash drives. The CODE image alone is not
a complete BIOS image for `-bios`. From the repository in WSL:

```bash
cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/fortress-test-vars.fd
timeout 20s qemu-system-x86_64 -M q35 -m 2G -display none \
  -serial stdio -monitor none -no-reboot \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=/tmp/fortress-test-vars.fd \
  -cdrom bin/fortress.iso
```

Timeout after the final completion message is expected because the kernel halts.

## Explicitly deferred

- Mapping failure rollback, empty page-table reclamation, and address-space
  destruction. Current tables are retained for the kernel address-space lifetime;
  unmapping does not release table frames or caller-owned data frames.
- Fault injection for allocator exhaustion and oversized boot metadata, and
  Ring 3 execution tests for effective user permissions.
- Actual stack-exhaustion testing through the double-fault emergency path.
  Writing directly to a guard verifies its mapping, not exception delivery when
  the interrupted stack itself has run out of space.

These items must not be described as completed failure-path or overflow tests.
