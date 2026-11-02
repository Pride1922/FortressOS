# Complete ACPI/desktop installer — 2026-11-02

Source release: `a12428a`, clean isolated checkout
`.codex-remote-attachments/acpi-desktop-release`.
Historical artifact (superseded by the [receive diagnostics image](wifi-rx-diagnostics.md)): `bin/fortress-base-installer.img`, 553648128 bytes (528 MiB).
SHA-256: `13283ee703b3fe9dfefc099a1ac3e2a81ef5f43d63c1be0f9357203ccfe88a9e`.
Build receipt: `bin/fortress-base-installer.manifest.json`; the isolated checkout
retains its original producer receipt. Publication copies identical image bytes
and relocates verified immutable package artifacts.

Includes the ACPI battery/AC-adapter/thermal telemetry changes (`adceb4e`,
`688bbec`), documentation (`a4b136f`), SYS_BATTERY_GET 103 and 24-byte status,
`/bin/battery`, desktop battery icon and Power settings, the shell command
completion commit, Wi-Fi handover and raw-RX corrections, services and four
factory desktop packages. Hardware telemetry and cold Wi-Fi ARP still require
physical Dell acceptance; these build results do not establish that acceptance.

## Fresh-build corrections

The normal four-binary Cargo build exposed a shared canvas dependency on the
compositor-only `ui` module. Battery pixel rendering now lives in shared styling;
the compositor imports it through `window::theme`, while the standalone app
canvas uses its own shared `theme` module. Pixel behavior is unchanged and the
existing widget API is re-exported. Compositor, Files, Terminal, Notepad and the
clipboard daemon all compile under the normal freestanding build.

Initramfs packaging also copied `battery.elf` without depending on its build.
An explicit USER_BATTERY_ELF prerequisite now ensures clean builds include it.
These integration fixes are committed in `bb2d59a`, `f00a969`, `5268710` and
`a12428a`; all are part of the release source.

## Verification

- `make test-battery-host test-abi-version-host test-iwl-mac-host`: PASS.
- Battery tests separately compiled/run under ASan/UBSan: PASS, including
  percentage/time/state calculations, AML battery methods, EC/cache mocks,
  AC adapter and thermal evaluation. This is not real EC/AML hardware evidence.
- Current compositor host tests: 62 PASS, zero failures, with the previously
  identified hanging `built_in_apps_share_focus_widgets_and_incremental_damage`
  case explicitly filtered. No full-suite PASS is claimed.
- Full isolated kernel/bootstrap/Cargo/installer build: PASS after the fixes.
- Factory package, independent filesystem, root expansion, GPT/FAT and payload
  audits: PASS through the standard producer.
- Bootstrap battery/compositor loadable bytes match the current build outputs.
  Strip removes the battery ELF's empty zero-length data PT_LOAD; nonempty
  segments and entry point are checked identical. Kernel symbols establish the
  battery, AC-adapter and thermal routines are compiled into the release.

Evidence is retained under `build/acpi-release-evidence/`; raw-image smoke logs
live in its `raw-smoke` directory. Raw USB checks attach no installation target
and only establish arrival at the installer, not a successful installation.
Only the published disk image is retained after verification. Physical storage
was not written and no production package repository was published.

Final raw-image BIOS/UEFI SMP=4 smoke PASS 2/2, reaching the installer
without a target disk. Image hash stayed unchanged. The private image duplicate
was removed and exactly one disk image remains in the project.
