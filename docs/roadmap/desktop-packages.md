# Desktop package factory provisioning — 2026-11-02

Four real application FPKs are provisioned into new installer roots as ordinary
closed package records, with exact retained predecessor sources. Installed base
bootstraps omit the three optional client binaries; live media retains them.
No collision policy, root display authorization or repository signature gate is
weakened. No production signing keys are generated or copied.

Host evidence:

- `build/desktop-packages-tqr8oxtp/manifest.json`: PASS. Actual FPK verification,
  ext4 package staging and record serialization; independent Linux filesystem,
  record, ELF, source-cache and configuration audits. Actual root expansion before
  four signed build-2 upgrades, predecessor/signature rejection with zero writes,
  idempotent repeat, and Linux e2fsck afterward. Test keys remain in the test branch.
- `build/desktop-cache-host.log`: PASS. Actual predecessor-source lookup uses the
  bounded split-hash path and retains its descriptor; absence/foreign metadata
  falls back to repository fetch, with cleanup checks.
- `build/desktop-app-compositor-host.log`: 55/55 PASS.

The factory helper refuses nonempty inventory, preserving existing installations
such as rot13. Tests write only disposable regular files and remove their images.
These checks do not establish guest GUI launch, production publication or physical
acceptance. Firmware/physical acceptance remains pending.

Final installer: `bin/fortress-base-installer.img` (528 MiB), SHA-256
`db550b3813bc3b98e2927eb9b2cb6dff49247e7eb2b5a44cefe76a0ca9d05d70`. Exact factory bundle: `bin/fortress-desktop-packages-b0ddcd21a3fd8d79`.
The image producer completed package, filesystem and media audits; no boot test.
