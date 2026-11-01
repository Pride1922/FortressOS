# Fresh installation with the new package CLI

**Historical build, withdrawn.** The user stopped the remaining verification and
requested deletion of all generated disk images on 2026-11-01. No installer image
is currently retained. A new image incorporating the latest Wi-Fi work and
redesign awaits separate instructions. The artifact names and commands below
record the discarded candidate; do not treat them as an available download.

Use `bin/fortress-pkg-fresh-kyr7gqvu-installer.img` for a fresh installation.
It contains the current kernel and the new `/bin/pkg`, configured for the existing
`https://fortress.ameyerocha.com` repository. No separate CLI or local web server
is required. Application inventory starts empty.

Flash this complete `.img` to the USB using the existing whole-device flashing
procedure. Boot it, select the intended installation disk by its displayed model
and serial, and complete account setup. **Installation erases the selected disk.**
Remove the USB and boot the installed system, then run:

```sh
pkg --help
sudo pkg install rot13
/usr/bin/rot13 --version
sudo pkg installed
sudo pkg upgrade
sudo pkg upgrade -y
```

There is no `pkg update` or `pkg get` prerequisite. Reboot and verify that rot13
still runs. Additional applications can be installed when published in the
existing signed repository.

Image SHA-256:

```text
ff5cac1936faa456284ef06831a65e410d43fc2deb4367606a1cedd7466d87b8
```

Build receipt: `build/root-phase4/pkg-fresh-kyr7gqvu/manifest.json`.
Acceptance logs: `build/root-phase4/pkg-fresh-accept-imxiog4a/`.
Physical acceptance remains pending.

The older updater's unsupported/inconsistent/wrong-version refusal performed no
writes. That predecessor-bound updater is superseded for this reinstallation.
Older generated disk images and ISOs were removed at the user's request; the
cleanup receipt is `build/disk-image-cleanup-2026-11-01.json`.
