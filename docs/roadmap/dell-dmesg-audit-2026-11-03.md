# Installed Dell log audit — 2026-11-03

Read the installed laptop at 192.168.0.194 over authenticated SSH. The old
known_hosts entry was removed after reinstallation; the new ED25519 fingerprint
matches the user's supplied SHA256:RkscqJP8T/gOM85ZrDT1N6HGRGLso1daMWO5tjDOtdk.
Credentials were used interactively and not saved into scripts or source.
Full and ordinary captures are local under `build/dell-dmesg-audit/`.

## Findings

Ordinary dmesg contained 533 lines, including 195 cumulative EXT4 PERF lines.
`SYS_SYNC` appended another complete performance snapshot on every call. This
wasted ring capacity and made successful background activity look like a fault.
The captured statistics report zero storage I/O failures/anomalies.

Wi-Fi RX: 217/217 target group frames decrypted, zero group decrypt/replay errors,
32-byte GTK; four local ARP requests and four successful replies. No unexpected
kernel panic or process crash appeared in the capture. These counters are one
snapshot, not a long-duration reliability claim.

The malformed-MADT warnings and zero-CPU MADT summary came from synthetic parser
fixtures. The real firmware MADT reports eight CPUs. Dummy severity samples even
emit emergency/critical messages on normal boots. No supported USB storage was
present, but the message said FAIL; descriptor provenance listed unrelated USB
interfaces and endpoints. The COM1 loopback warning, 32 GiB PMM ceiling and
unavailable AML battery telemetry remain genuine limitations, not cleanup targets.

## Corrections

- Successful sync no longer publishes ext4 profiling into the persistent ring.
  SYS_DMESG produces a single bounded current ext4 snapshot in the validated root
  caller's output, with debug severity. A temporary 2 KiB kernel allocation avoids
  user-buffer writes under the ext4 lock; allocation failure preserves log/RX output. Ordinary dmesg hides it; `dmesg -v` shows
  it. The syscall number, signature, root requirement and cap clamp are unchanged.
  RX snapshots still finish the output, preserving the existing tail workflow.
- Remove per-file TarFS and USB descriptor provenance dumps; retain mount totals
  and enumeration/error handling. USB storage absence is an informational message;
  detailed no-storage context is debug. DMA ownership/quarantine is unchanged.
- MADT buffer parsing no longer logs fixture successes/rejections. The firmware
  wrapper reports actual validation failures and the real successful topology.
  Validation rules and boot tests still run unchanged.
- Synthetic severity samples require the explicit QEMU `dmesg_test` fw_cfg key.
  Numbered `[TEST 1]` messages are classified as debug like other test headings.
- Headless dmesg integration tests build private text-console media so the newly
  committed graphical greeter does not replace their serial login. The ext4 USB
  profiler test explicitly asks for verbose diagnostic output.

## Verification

Native strict compilation covers the modified kernel, filesystem, ACPI and USB
objects. ASan/UBSan dmesg CLI filtering passes. The new actual-SYS_DMESG host
regression covers 4,097 capacities, oversized-cap clamping, destination canaries,
invalid user-page rejection, allocation-failure fallback without leaks, and
root-only profile access, using mock hardware
counter formatters and user-page adapters. It does not establish hardware/SMP
behavior. Run `make test-dmesg-filter-host` to include it.

BIOS and UEFI SMP=1 QEMU integration passes the real text login, sudo elevation,
facility/severity/tail filtering, log-file output and shutdown sequence. QEMU
attaches a disposable regular-file NVMe copy; it never accesses physical storage.
The full ISO build passes. Retained initial failures include the headless test
selecting the greeter, followed by a test-helper Path conversion error; both are
corrected in the successful final runner. A separate error-status prompt race
was eliminated by capturing a labelled status in the same command.

The broader `test-perm-phase5-host` was attempted but its syscall fixture does not
link against current APIs: missing process identity spawning, RX snapshot and
network-owner adapters (plus the new ext4 snapshot adapter). The dedicated
SYS_DMESG regression above supplies explicitly scoped adapters and passes; the
broader harness is not reported as passing. Exact failure is retained locally.

Changes are source-only; the installed Dell and published 64f16bd installer still
run the previous kernel. No laptop files, drivers or services were changed.
