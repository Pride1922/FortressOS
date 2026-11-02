# Single complete installer — 2026-11-02

## Current diagnostic media: live USB

Use `bin/fortress-live.img` (130 MiB), SHA-256
`16a06a3475f13b71558bca6f91281a2adb8a240c5cfac8a5d598c068df7913b4`.
This boots the ordinary live login and includes the latest committed ACPI battery
multi-table fix, desktop changes and Wi-Fi diagnostics. BIOS/UEFI live USB tests
PASS 2/2. The installer-only file below has been retired, leaving one published
image. [Live commands, source identity and evidence](acpi-live-usb.md).

## Earlier installer: Wi-Fi receive-stage diagnostics

`bin/fortress-base-installer.img`, 528 MiB, SHA-256
`09ab399d209efae3938458519719c9d15754ad1ee7e92c66854ca847b01a58cf`.
Clean source `d9be498`; includes the prior committed ACPI, desktop, completion
and network work. `sudo dmesg -n 4` now shows raw radio, group decrypt,
decapsulation and ARP reply counters on BSP/AP without packet-path logging.
Host sanitizer/concurrency gates, BIOS/UEFI × SMP=1/4 guests and final raw-image
BIOS/UEFI boot smoke PASS. Cold ARP still failed on the preceding ACPI image;
this replacement is diagnostic, with physical diagnosis pending.
[Commands, limits and artifact evidence](wifi-rx-diagnostics.md).

## Earlier image: ACPI power telemetry and desktop integration

`bin/fortress-base-installer.img`, 528 MiB, SHA-256
`13283ee703b3fe9dfefc099a1ac3e2a81ef5f43d63c1be0f9357203ccfe88a9e`. This image includes the ACPI battery/AC-adapter/thermal
work, SYS_BATTERY_GET, `/bin/battery`, desktop Power settings and battery icon,
command completion and the Wi-Fi corrections. It is built from clean isolated
source commit `a12428a`. [Build fixes, tests and limits](acpi-desktop-release.md).
Final raw-image BIOS/UEFI boot checks PASS 2/2; physical acceptance remains pending.


## Earlier candidate: cold-ARP receive configuration

The sole published installer is `bin/fortress-base-installer.img`, 528 MiB,
SHA-256 `0688b23e6ab8bb74d221876c213ba85f172afbc13ae9cdfb778d8a57f21a57cc`. It is built from clean isolated commit
`85e8484`, including command-completion commit `8bc30ad`. Parallel uncommitted
work is not included or modified. The actual Intel MAC_CONTEXT serializer now
requests raw unicast/group reception for host software decryption.
[Physical ARP isolation, command-byte tests and remaining limits](wifi-cold-arp.md).
Host regressions and BIOS/UEFI × SMP=1/4 ownership guests PASS. These do not
establish Intel physical reception; cold ARP on the Dell remains pending.
Raw-image boot evidence: `build/wifi-cold-arp-evidence/raw-smoke/`.
The original isolated producer receipt remains in that checkout; publication
only relocates the identical audited image/package artifacts.

## Earlier replacement: Wi-Fi interface-flapping fix

The sole current image is `bin/fortress-base-installer.img`, 528 MiB, SHA-256
`98bf2b01b2d9085e3859bd2e0afab81b61512eb9662a622c3b6a3459d3f38df6`. It replaces the earlier image described below.
It includes `1c2b880`: disconnected Ethernet no longer evicts recovering Wi-Fi,
and automatic retries no longer restart an authorized/in-flight connection.
[Cause, before/after regressions and physical limits](wifi-interface-flapping.md).
The kernel and C tools were force-rebuilt through the Ubuntu container; the
bootstrap was regenerated after correcting ownership of an old generated
firmware directory. The image receipt records a clean committed source tree.
Host regressions PASS; BIOS/UEFI × SMP=1/4 mock-radio ownership tests PASS 4/4.
Raw-image BIOS/UEFI SMP=4 boot checks PASS 2/2; results are in
`build/wifi-handover-evidence/raw-smoke/`. Physical
verification of this replacement remains pending. The image is not deployed
merely by rebuilding it.

## Earlier installer evidence (superseded image)

Use `bin/fortress-base-installer.img` (553648128 bytes, 528 MiB).
SHA-256: `8afbab95e23d05360d45a53bf3e45dd16d16a5de2beff6e5164de53cf6b5736b`.
The build receipt is `bin/fortress-base-installer.manifest.json`.

The complete A/B installer includes the committed Wi-Fi credential-retention,
security-selection and cold-start retry fixes (`cf68155`), Intel firmware,
enabled Watchtower Wi-Fi auto-connect, the base services and all four factory
desktop application packages. The current desktop, sudo and package working-tree
changes are included; they remain separate from the Wi-Fi commit. Exact source
hashes and that pre-existing working-tree state are recorded in the receipt.
No production repository was published and no physical drive was flashed.

The normal kernel/bootstrap were built through the existing Ubuntu 24.04
`fortress-dev` container, followed by `scripts/create_base_installer.py`.
The producer passed factory FPK verification, independent filesystem/package
record/source-cache audits, root-expansion checks, and GPT/FAT/payload checks.
Loadable ELF segments of the bootstrap's Wi-Fi, sudo and compositor binaries
match the current build outputs; stripping only removes non-runtime sections.
The bootstrap contains the Intel firmware and enabled `/bin/wifi auto` service.

Verification:

- Wi-Fi profile, manager, fault, firmware-preparation and stack host gates PASS.
- Actual owner ASan/UBSan and TSan tests PASS (4800 concurrent calls each).
- `scripts/test_wifi_owner.py --cpus 1 4 --firmware bios uefi` PASS 4/4 in the
  container. Actual Ring 3 calls, copied owner code and mock radio; no Intel RF
  or hardware DHCP claim. Evidence: `build/wifi-owner/qemu-qona_p_u/`.
- Sudo spawn tests PASS. Of 62 compositor host tests, 61 PASS; the existing
  `built_in_apps_share_focus_widgets_and_incremental_damage` host case hung and
  was terminated. The remaining 61 pass with that case explicitly filtered.
  No full compositor-suite PASS is claimed.

Final raw-image BIOS/UEFI × SMP=4 boot checks PASS (2/2). Evidence is in `build/final-installer-smoke/`. These
read-only USB boot checks attach no installation target, intentionally reach the
installer's unavailable-disk result, and do not establish an installation or
physical Wi-Fi acceptance. An initial BIOS attempt waited for the wrong exact
failure-screen text; its timeout and serial log are retained separately. The
corrected marker requires the installer and explicit zero-installation-write
result. The image hash is checked unchanged after each boot.

Cleanup is restricted to regular, untracked generated disk images beneath
`bin`, `build` and `.codex-remote-attachments`, with open-file and path checks.
The retained image is excluded. Removed 171 obsolete images, freeing 21570834432 allocated bytes (20.09 GiB);
only the final installer remains. Counts and removed paths are recorded in
`build/image-cleanup.json`; source files, test logs, manifests, firmware and
physical devices are preserved.

After a fresh installation, connect once with the actual network password:
`sudo wifi connect Halloween '<password>'`. Subsequent installed-root boots use
the saved profile automatically. Reinstalling replaces the target filesystem;
these fixes cannot recover an already erased password. Physical confirmation
still requires DHCP/DNS followed by automatic reconnect after reboot.
