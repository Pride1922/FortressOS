# First ext4 performance release — 2026-11-03

Implements the first two recommendations from the
[performance investigation](ext4-metadata-performance-investigation.md).
Production source checkpoint: `c33d397ce975e2acb563634c7a8027d485885d58`.

## Changes

Installer partition reads/writes expose optional bulk callbacks bounded to
4096 bytes. Full partition-relative and absolute-LBA bounds are checked before
identity validation or device I/O. Each accepted request still validates the
complete selected target identity. No failed run is retried as single sectors.

JBD2 recovery preview accepts bounded reads and applies all committed unrevoked
images, later replacements and the synthetic empty-journal superblock over the
complete returned byte range. An underlying run failure returns immediately.
The ext4 preview adapter exposes that path. Single-sector compatibility and
sub-sector filesystem-block handling are retained.

A shared immutable 1 KiB CRC32C table replaces bitwise CRC in ext4, JBD2 and
installer expansion. Raw seed/chaining and stored byte order are unchanged.
No CPU extension, SIMD state, lazy initialization or dynamic table allocation is
introduced. The kernel compiler optimization level is unchanged.

Cache size, validation coverage, journal sequence/flush ordering, payload chunk
size, per-request target identity checks and DMA ownership remain unchanged.

## Measurements

The paired host experiment uses the same factory root expanded to the Dell's
255991291392-byte geometry, three alternating before/after pairs pinned to the
same host CPU, with `-O0` matching the default kernel optimization level. These
are host timings, not laptop timings. Evidence: `build/ext4-performance-fix/`.

| Workload | Before | After |
| --- | ---: | ---: |
| Backend read requests per mount | 73160 | 16284 |
| Metadata bytes read per mount | 66647040 | 66647040 |
| CRC bytes checked per mount | 37776396 | 37776396 |
| Median mount time | 2323.939 ms | 1805.496 ms |
| Median 100 lookups + metadata queries | 44.332 ms | 10.313 ms |

The installed bulk-device path issues 4.49× fewer read requests in this mount
workload; the preview's sector amplification is removed. Validation processes
the same bytes. Runtime lookup I/O stays at 480 requests because directory/cache
policy is unchanged; the 4.30× lookup improvement comes from checksum processing.

Instrumented QEMU BIOS installations use the same published factory payload,
matching instrumented kernel banks and recomputed manifests, plus a regular-file
512 MiB target. Identity checks and validation remain active in both runs.

| Stage | Before | After |
| --- | ---: | ---: |
| Root provisioning IDENTIFY submissions | 82404 | 11388 |
| Root provisioning elapsed | 12.600 s | 2.188 s |
| IDENTIFY count entering readiness stage | 626057 | 555041 |
| Payload-stage IDENTIFY submissions | 493065 | 493065 |
| Payload-stage elapsed | 77.171 s | 86.766 s |

Provisioning is 5.76× faster in these runs with 7.24× fewer identity commands.
Payload copying is unchanged and remains dominant. Its timing varied with the
host run conditions; overall installer timing does not establish a comparable
large end-to-end speedup. Per-request identity/chunk redesign is still a separate
follow-up, with its existing safety contracts retained.

The first after-only host run overlapped other test jobs and had worse whole-mount
wall time despite fewer requests. It is retained in `host-after-*`; the alternating
same-CPU paired measurements above avoid comparing those different conditions.

## Verification

- ASan/UBSan standard CRC vector plus 3000 seeded, unaligned and streaming
  differential cases against an independent bitwise reference.
- Actual JBD2 preview with a synthetic validated plan: 512/4096-byte sectors,
  1/4 KiB filesystem blocks, overlaps, revoked/superseded images, synthetic journal
  superblock, overflow/boundary/canary checks and failed-run no-retry behavior.
- Actual private installer callbacks with mock identity/device adapters: one
  identity validation per bounded run, refusal of changed identity, no I/O before
  complete bounds validation and no read/write retry after failure.
- Linux-generated journal replay oracle, byte comparisons of pre-replay preview
  images, corruption/feature rejection, read/allocation failure unwind, idempotence
  and replay interruption across eight sector persistence/cut patterns.
- JBD2 writer 12/12 geometry cases including atomic crash cuts, tears, ordering,
  barriers, revokes and poisoned-state refusal. A second complete run uses bulk
  callbacks for 512-byte devices, deliberately accepting a sector prefix before
  injected failure; 4096-byte devices retain sector fallback coverage.
- Native strict compilation and clean-checkout complete installer production,
  including all five factory desktop packages and Linux root/expansion audits.

The legacy JBD2-only host targets needed current permission dependencies and
explicit exclusion of the unrelated package executor to build their standalone
fixture. Production package compilation/admission remains enabled and is exercised
by factory seeding. Initial harness compiler/link failures are retained in logs.

## Candidate and physical acceptance

Installer: `bin/fortress-base-installer.img`, 528 MiB (553648128 bytes).
SHA-256: `e4a2650597c39117f00682da993fc481a09395f37f3e6dea40ab402599393f92`.
This is a complete A/B installer. It includes committed desktop/Calculator,
networking, quiet-log and opt-in boot-self-test changes. Parallel uncommitted ACPI
and other work is excluded from this clean checkpoint.

Physical Dell performance acceptance is pending. After installation, time power-on
to login separately from Wi-Fi readiness. Record installer preflight, payload,
provisioning and final verification separately when visible. Run `ls /usr/bin`,
open the desktop apps, reboot without manually reconnecting Wi-Fi and test ping.
Capture `sudo dmesg -v` after login for the current ext4/RX snapshot. Real battery
telemetry remains unavailable in this checkpoint.

Release acceptance uses the final uninstrumented image for BIOS/UEFI USB
installation. Each installed target then boots twice under SMP=4 without USB;
SSH authenticates with the fixture account, saves/reads a note, lists the factory
apps, elevates and shuts down. All five installed app ELFs are byte-compared with
their factory FPK members, and the root receives a Linux `e2fsck -fn` audit.
The original authenticated boot menus and default graphical boot path are retained.
This is storage/login acceptance, not physical GUI interaction acceptance.

A failed initial serial harness edited menus to add `console=text`; base boot-set
validation correctly rejected the edited target. The attempted restoration also
changed the menus' FAT size/placement. That disposable target was retired and
installation repeated on a fresh disk. A subsequent SSH harness timeout was a
raw-versus-ANSI-stripped offset bug, corrected in the host harness. Logs remain;
production boot-menu validation was not weakened. An unclean test stop during
that timeout is covered by the later successful reboot/clean shutdown/fsck audit.

Final release acceptance: **PASS** for both BIOS and UEFI installations and all
four installed SMP=4 boots, note persistence, five byte-exact factory app ELFs
and Linux filesystem audits. Receipt: `build/ext4-performance-fix/acceptance/manifest.json`.
