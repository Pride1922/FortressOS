# Live USB with latest ACPI work — 2026-11-02

Current published live image: `bin/fortress-live.img` (130 MiB), SHA-256
`bcac8b695122ac0a2b9356adee7b94df4aae2f337f1aab70377f8bb4d1b626db`; production source `d9a682d`.
The predecessor below physically reported false battery/AC readings. The current
image explicitly reports telemetry unavailable; actual battery/adapter/thermal
support is **incomplete**. [Failure, production regression and correction](battery-telemetry-unavailable.md).
BIOS/UEFI live login and explicit unsupported CLI output PASS 2/2, with Wi-Fi
diagnostics and desktop version checks retained. No new flash is needed merely
to continue the ongoing Wi-Fi test.

## Earlier live image (superseded, false hardware readings)

Earlier diagnostic media: `bin/fortress-live.img`, 136314880 bytes (130 MiB).
SHA-256: `16a06a3475f13b71558bca6f91281a2adb8a240c5cfac8a5d598c068df7913b4`.
Receipt/checksum: `bin/fortress-live.manifest.json` and
`bin/fortress-live.img.sha256`. This is live-only media, booting the ordinary
operator login rather than the installer. No OS installation is needed.

Clean source `fde4e268b9714097753a25ad3daec24f54416b5f` includes ACPI battery,
AC-adapter/thermal telemetry, the latest multi-table battery discovery fix,
desktop Power settings, Notes Open/Save changes, command completion, and the
Wi-Fi receive-stage diagnostics. The recovery/live desktop and bundled clients
are in the initramfs. Live desktop invocation is `sudo /bin/compositor`.

Boot the USB and log in as `operator` (passwordless live default). To inspect
power telemetry and collect receive diagnostics:

```sh
battery
sudo dmesg -n 4
```

For the cold-ARP test, connect Wi-Fi once, report its current IP and capture the
four diagnostic rows before pinging Bazzite. Repeat after the measured ARP burst.
Live configuration is not an installed-root persistence test.

## Build and verification

The isolated checkout is `.codex-remote-attachments/acpi-live-release`.
The normal kernel/initramfs and battery host tests were rebuilt there using
the Ubuntu development container, followed by:

```sh
python3 scripts/create_boot_img.py bin/fortress-live.img
python3 scripts/create_boot_img.py bin/fortress-live.img --verify
```

The custom output name selects the existing live-only builder; the default
`bin/fortress.img` name would select combined live/installer packaging instead.
The generated configuration explicitly selects `boot_mode=live`. GPT CRCs,
FAT32 loader and journaled EXT4 data partition checks pass. Independently read
FAT kernel/initramfs bytes match the fresh outputs exactly. Kernel symbols
confirm ACPI table enumeration and battery APIs plus all three diagnostic
formatters; the archive contains battery, compositor and Notepad binaries.

BIOS and UEFI raw-USB boots under QEMU SMP=4 PASS 2/2: actual live login,
battery CLI execution, all four RXDIAG rows, desktop version output and shell
command execution; no installer startup or process fault. Only the regular-file
USB source was attached with disposable snapshot writes, with no installation
target or physical disk. The source hash remained unchanged. QEMU has no laptop
battery, so this validates command execution, not physical telemetry accuracy.
Battery host tests PASS all six groups. Physical ACPI and cold-ARP diagnosis
remain pending.

Evidence is retained under `build/acpi-live-evidence/`, including source/payload
identity, exact QEMU arguments, serial logs and boot manifest. The first BIOS
check incorrectly expected the word compositor in the version banner; the
actual banner is FortressOS Desktop 0.2.0. The assertion was corrected and both
firmware modes rerun without changing image bytes; that initial log is retained.

The earlier installer-only image and this build's duplicate were removed after
successful publication. Only `bin/fortress-live.img` remains as published disk
media. The superseded installer receipt is archived with the evidence.
