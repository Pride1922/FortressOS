# Installer startup diagnostics (2026-10-30)

User reports: Dell 5590 NVMe and Dell 5510 SATA are unavailable in the installer.
The displayed EOPNOTSUPP explanation previously suggested unsupported disks,
but the same result occurs when no installer backend was registered. This can
happen before disk discovery: source provenance, typed payload validation,
existing storage ownership and boot-mode admission all precede enumeration.
There is no shell in the installer. Physical root cause is not yet established.

The failure screen now includes the last six relevant kernel log lines on
stage-one failure, using the existing root-only SYS_DMESG snapshot. It matches
AHCI, NVMe, STORAGE and INSTALL messages; bounds each displayed line to 160
bytes, uses static snapshot storage, and handles unavailable logs silently.
Other log categories are omitted. Existing disk identity, provenance, source
exclusion, leases, write authority and DMA quarantine checks are unchanged.

Kernel startup rejection messages distinguish invalid provenance, invalid payload,
already-owned storage, invalid boot mode, absent/incomplete inventory and failed
identity/read checks. Combined discovery reports AHCI rejection explicitly: a
failed AHCI inventory withholds the whole combined list, including NVMe. This is
a possible failure path, not a confirmed explanation of either physical report.

Verification: `make test-install-cli-host test-ahci-host
test-install-control-host` PASS under the existing host sanitizer runners.
CLI coverage checks relevant diagnostics, exclusion of unrelated wireless log
contents, latest-six-line bounds, a final unterminated line and unavailable logs.
Two stale test expectations said “MiB NVMe”; the existing production UI says
“MiB disk”, so those expectations were corrected without changing disk labels.
Driver tests use mocked hardware; no new physical acceptance is claimed.
`make -j4` PASS; combined-image verification succeeded before publishing
`bin/fortress.img` (592 MiB), build `build/root-phase4/media-jrxkb2pi`.

The existing live USB can provide read-only AHCI/PCI logs without reflashing.
Ordinary live boot does not necessarily initialize NVMe, so lack of NVMe log
output is not evidence of absent hardware. Installer-specific startup failures
require the diagnostic installer screen.

## Reproduced common startup rejection and packaging fix

The UEFI SATA virtual installation (`build/root-phase4/sata-3z5ov8gk`) reached
the same generic unsupported-configuration screen before discovery, with the
new diagnostic: `installer payload module set invalid`. The private installer
initramfs was 8,622,080 bytes. Kernel typed-module admission and archive
personalization cap it at 8 MiB (8,388,608), while the media producer incorrectly
allowed 16 MiB. This independently reproduces a packaging regression applicable
to both disk transports, without proving the physical laptops took this path.

The producer now strips debug sections from a private copy of the installer ELF
(431,944 -> 75,112 bytes), verifies the entry point and all PT_LOAD segment
addresses/flags/sizes/alignment/content are unchanged, and packages that copy.
The original binary is preserved. Publication now enforces 8 MiB with 2,560 bytes
of headroom for the personalized recovery record. Kernel admission, archive
validation and installed bootstrap slot sizes are unchanged.
The corrected combined image is `build/root-phase4/media-1kxiurbw`, with an
8,263,680-byte installer bootstrap, published as `bin/fortress.img` after normal
media verification. The UEFI SATA rerun uses private media `media-_s2h8cj7`;
it passes startup, identifies the SATA target and validates the installer files.
Final rerun `build/root-phase4/sata-sokxx057` PASS: UEFI SATA installation on a
private 384 MiB QEMU disk, post-install Linux audit, installed-root I/O and clean
shutdown at SMP=1 and SMP=4. No physical disk was attached. This validates the
packaging correction through installation, personalization and reboot, rather
than only bypassing startup.

## Physical confirmation (2026-10-30)

After testing the corrected image, the user reports “both machines is a pass”
in response to the requested disk-selection checks: Dell Latitude 5590 with NVMe
and Dell Latitude 5510 with SATA. Installer startup/disk detection therefore
passes on both physical machines. This report does not establish completed
physical installation, reboot persistence, performance or fault recovery.

An earlier virtual test attempt (`sata-tjbh_5ve`) failed during fixture production
before launching a guest; its captured exception did not retain producer stderr.
A standalone producer rerun succeeded; no hardware conclusion is drawn from
that setup failure.
