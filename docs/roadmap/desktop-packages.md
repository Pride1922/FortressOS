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
`0688b23e6ab8bb74d221876c213ba85f172afbc13ae9cdfb778d8a57f21a57cc`. Exact factory bundle: `bin/fortress-desktop-packages-4d2b5827126f6171`.
The image producer completed package, filesystem and media audits; no boot test.

Production cutover: catalog revision 3 is now published with a newly generated
private server-side key. Public HTTPS verification confirms that all four offered
app artifacts exactly match the production-trusting installer. The old public
key rejects this catalog. Dell install/upgrade acceptance remains pending.

## Foreground launcher and restart (2026-11-02, local implementation)

`sudo desktop` now searches the root-controlled package command directory. Fixed
search ordering, directory rejection, relative/oversized-name refusal, ignored
caller PATH and full invoking-user environment are checked by the actual sudo
host harness. The waiting sudo parent recognizes only the foreground packaged
desktop's explicit restart exit status; it respawns with the same identity data.
Normal exits/errors are not restarted; restart loops are bounded.

The desktop menu adds a process restart action, gated on the launcher marker and
no active/exiting external applications. All input/display cleanup occurs before
exit; new initialization and privilege drop occur on the new process. Watchtower
is unchanged. No new kernel or desktop-client protocol is introduced.

Evidence: `make test-perm-spawn-host` PASS, including full sudo desktop resolution,
two requested relaunches and loop-limit refusal; `make test-compositor-host` 62/62
PASS, including launcher-marker parsing, actual menu click and active/exiting-app
restart refusal. Logs: `build/sudo-package-command-host.log` and
`build/desktop-restart-host.log`. Normal sudo/compositor/CLI builds are required;
no firmware or physical restart acceptance is claimed. Nothing has been published
for these changes, and the existing Dell still needs an updated base launcher.
