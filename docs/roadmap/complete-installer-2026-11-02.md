# Single complete installer — 2026-11-02

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
