# Complete desktop and Calculator installer — 2026-11-03

Candidate: `bin/fortress-base-installer.img`, 528 MiB (553648128 bytes).
SHA-256: `6296e1610d3405c156d8341e506a67cde2a51043e7607a5f0d0ebe86cb4a81e9`.
Built from clean isolated source `64f16bddb84a6d888b2d3897bdb73f209a0672f9`.
This is an installer, with complete A/B base-update slots, not live-only media.

Includes the committed libui SDK/design and Calculator (`03756cc`),
[terminal resize correction](terminal-resize-stride.md), Wi-Fi GTK delivery,
scoped profile durability, DHCP contention handling, command completion, and
ACPI/desktop Power work. Battery hardware telemetry remains explicitly unavailable;
no fabricated battery percentage, AC status or temperature is restored.

The factory seeder previously hardcoded four package names and positional CLI
arguments. It now seeds and checks all five apps in the producer's package-name
order: Calculator, Desktop, Files, Notepad and Terminal. Counts and upgrade offsets
are derived from the fixture array. Calculator is excluded from the base bootstrap
alongside the other independently packaged apps, and installed as `/usr/bin/calc`.
Runtime package signature and admission rules are unchanged.

Host verification: libui 8/8 and compositor 72/72 PASS. The factory suite verifies
all five FPKs, installed records and source retention using actual VFS/ext4/package
code under ASan/UBSan. All five authenticated upgrades pass on a separate test
copy; foreign predecessors and invalid signatures cause zero-write refusals,
and verified repeats are no-ops. Linux filesystem and expansion audits pass.
Ephemeral test signers are not provisioned into the release.

Installer acceptance PASS on both BIOS and UEFI (two installations and four
installed boots). The published raw image hash remained unchanged.

Installer acceptance uses the final raw USB bytes and the ordinary installer,
with one disposable regular-file target, no physical devices and no disposable
installer bypass. BIOS and UEFI each install with SMP=1, then boot twice with
SMP=4 from the target alone. A home-directory note must survive the second boot;
all five `/usr/bin` app ELFs are compared byte-for-byte with their FPK members,
and the installed root is checked independently with `e2fsck -fn`.
The source image hash is checked before and after the campaign.

Release build and verification logs: `build/dell-nov3-release/`. The image receipt
and checksum sit beside the published image. Physical installation, saved Wi-Fi
reconnection and graphical usability remain Dell acceptance gates; use the
[short Dell checklist](../dell-acceptance-2026-11-03.md).

## Dell user acceptance — 2026-11-03

The user reports PASS for wrong-password rejection, correct-password connection,
window resizing, outbound ping, installed reboot persistence and cold-power-on
auto-connect. Bazzite cleared its neighbour entry before probing FortressOS at
192.168.0.194: 3/4 replies initially, with no static neighbour workaround;
FortressOS also reached 1.1.1.1. The subsequent requested cold-boot/stability test
was user-reported PASS without a transcript. This is bounded user-confirmed
acceptance, not a zero-loss or long-duration reliability claim. Calculator's
arithmetic, credential permissions and power telemetry were not separately
confirmed by these reports.

Post-image quality-of-life correction: remove routine kernel WLAN success,
EAPOL progress and active-interface announcement prints. Authentication rejection,
key validation failure, disconnect and timeout diagnostics remain; `wifi status`
and on-demand RX counters remain available. No connection, DHCP, key handling,
CPU-0 ownership or driver behavior changes. This source-only correction is not
in the published 64f16bd installer. Actual-code profile, radio and owner host
checks pass (including 21 radio cases and 4800 concurrent owner calls); strict
native compilation covers both modified objects. Evidence: `build/wifi-quiet-console/`.
