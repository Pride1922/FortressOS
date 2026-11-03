# Complete installer with boot phase timings — 2026-11-03

Release source checkpoint: `11df26df11bee0dd1360cfc1f77db512ba1b4983`.
The clean build includes all committed ACPI, desktop, Vault/terminal, input/power,
network and storage work, including the gterm entry-stack fix (`ab3cefa`).
The release banner is `boot-timing-2026-11-03`.

## Measurements

`[BOOT TIME]` records give elapsed integer milliseconds from the boot-calibrated
monotonic TSC. They use no wall clock/NTP or counter reset. Labels and samples
are bounded to sixteen phases per local trace; truncation and invalid clocks
are explicit. Collection adds no allocation, media I/O or locks. Emission uses
the existing serial/dmesg logger only after the measured function releases locks.
Boot initialization order, target identity/admission checks, replay, publication,
cache coherence, DMA ownership and every durability barrier remain unchanged.

- **KERNEL:** early platform work, scheduler, discovery, SMP, input/USB,
  networking and persistent storage. An early raw TSC sample is converted only
  after calibration. Total covers `kmain` entry to kernel-ready; it excludes
  firmware/bootloader time and later greeter rendering.
- **ROOT:** boot credentials/readiness, bootstrap CLI, device inventory,
  selected-disk/GPT checks, complete base boot-set validation, flush preflight,
  filesystem preparation, installed-system validation and publication.
- **EXT4-MOUNT:** workspace, journal source/analyze, recovery preview, replay,
  reopened ownership validation, activation, orphan recovery, barrier and root.

Scopes are nested: ROOT includes EXT4-MOUNT; KERNEL includes ROOT. Do not sum
those totals. Individual durations round down to milliseconds. Root rejection
and clock rollback are reported explicitly. These records describe that mount;
rereading `dmesg` does not turn them into accumulated runtime counters.

After installing and connecting Wi-Fi, collect over SSH or run:

```sh
/bin/sudo /bin/dmesg -v
```

The earlier Dell baseline is installation 135 seconds and three cold boots of
21, 20 and 19.85 seconds (median 20). The earlier log's accumulated bitmap and
checksum counters could not isolate mount duration. The new measurements can.
In the first small QEMU installed boot, ROOT base boot-set validation was 4700 ms
while EXT4-MOUNT totaled 133 ms. That illustrates the separation; it is not a
Dell timing or a physical speedup claim.

## Verification

- Sanitized deterministic timing test: deltas/totals, early sample calibration,
  bounded phase list, zero/invalid clock, rollback and integer overflow.
- Strict freestanding kernel build; actual APIC clock references are present.
  Host fixtures use the hosted-environment switch, rather than the Linux target
  macro (Linux GCC also defines that macro when compiling a freestanding kernel).
- `make test-gterm-stack`: compiled entry frame 568 bytes; the old crashing
  binary fails this gate. The unchanged compositor/terminal code passed 80 host
  tests in the preceding fix.
- Actual runtime deployment and independent Linux FAT/ext4 audits: PASS.
- Final image: **PASS BIOS/UEFI USB installations**, each followed by two
  installed SMP=4 boots without USB. All three timing scopes and required ROOT
  phases appear with valid numeric elapsed times; the release marker matches.
  Normal-user gterm entry without a compositor exits gracefully with status 2,
  and no process crash appears. Saved-note persistence, five byte-exact factory
  application ELFs and Linux filesystem audits pass in both firmware profiles.
  This verifies entry safety, not full graphical-window interaction.
  Receipt: `build/boot-timing-release/acceptance/manifest.json`.

The first clean producer attempt exposed a missing output-parent directory;
that producer now creates it. The next attempt exposed duplicate input-device
no-ops in nested host fixtures; those now reuse the common filesystem fixture's
stubs. Initial failures are retained in `build/boot-timing-release/`. Those fixes
do not change runtime device handling or permissions.

The published image is replaced only after final image acceptance. No physical
disk is written by the build/test process. The flash/install steps are user-run.
Battery telemetry remains a physical open issue; the preceding SSH check failed
to read it. Plain sudo and sudo -s worked in that check; their earlier startup
failure was not reproduced and no credential policy was changed.


## Published artifact

`bin/fortress-base-installer.img`, **528 MiB (553648128 bytes)**.
SHA-256: `c90403dcf73940630b334cc0021882a5af553f8e47c25256c3783d287d0c2063`.
This is the single complete A/B installer, replacing the earlier published image
only after final acceptance. Kernel banner: `boot-timing-2026-11-03`. All build and
acceptance I/O targeted regular files; no laptop or USB hardware was written.

Build/host/producer logs and test receipts are under `build/boot-timing-release/`.
The private clean release checkout is `.codex-remote-attachments/boot-timing-release`.
The producer's original output-parent error and nested fixture compile failure
are retained; the successful rebuilt source includes their corrections. The
first timing build's hosted/freestanding detection was corrected before creating
the accepted candidate. Actual installed-guest timing output verifies that this
release enables the instrumentation in the freestanding kernel.


Dell physical timing capture is now available: [actual phase breakdown and
priorities](dell-boot-timing-2026-11-03.md). Root startup is 16060 ms, including
7727 ms base boot-set validation and 4281 ms ext4 preparation. Kernel-ready is
27780 ms; this is not firmware-to-login timing.
