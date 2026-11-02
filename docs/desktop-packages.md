# Desktop packages

The complete base installer includes `fortress-desktop`, `fortress-files`,
`fortress-notepad` and `fortress-terminal`, each with an ordinary installed record.
Notes is a Notepad launcher, with a temporary `/tmp/notes.txt` default.
Wi-Fi, firmware and SSH server are base components; independent service packages
are not implemented.

From the console, inspect and update through the configured signed repository:

```sh
sudo pkg installed
sudo pkg upgrade
# Noninteractive acceptance of the displayed upgrade plan:
sudo pkg upgrade -y
sudo desktop
```

Stop the graphical desktop before replacing its applications. Base updates are
staged after application updates and require a reboot. See
[base updates](base-repository-updates.md) for confirmation and recovery.
Configuration and user documents are outside package payloads.

## Producing releases

Build the normal kernel/initramfs and installer helper, then run:

```sh
python3 scripts/create_base_installer.py
```

This produces `bin/fortress-base-installer.img`, its source/hash receipt, and an
immutable `bin/fortress-desktop-packages-<identity>/bundle.json` with the exact four
factory FPKs. These are unsigned artifacts. Transfer the exact bundle to the
repository signer; publish its original build numbers and hashes unchanged.
A changed artifact requires a new monotonic build number.

For a later individual application release:

```sh
python3 scripts/build_desktop_package.py --app notepad \
  --platform-id "$FORTRESS_PLATFORM_ID" --build 2 \
  --base-min 1 --base-max 1 --out build/fortress-notepad-2.fpk
```

Use the existing repository identity, signing key and next catalog counter on the
repository server. Retain existing catalog entries, publish immutable objects
first, then atomically activate the signed catalog. Never generate replacement
trust or sign with demonstration keys to bypass production enrollment. See
[repository operations](pkg-repository-ubuntu.md).

Factory records have no catalog authority: the installer supplies their initial
ownership and hashes. Cached predecessor FPKs allow the first signed upgrade;
the kernel still verifies their exact hash against the installed inventory.
Runtime installation and upgrades retain normal signature and admission checks.
