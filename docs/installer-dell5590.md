# Dell 5590: install on the authorized spare NVMe

2026-10-12. The user authorized erasure of the identified PC401 NVMe SK hynix
256GB spare and the bounded layout. The user confirmed installation and first boot without USB on 2026-10-12.
Clean-shutdown persistence also passed; recovery and independent audits remain pending. The instructions
below describe the revised installer; the existing installation need not be
replaced to continue acceptance.
The image accepts only inspector digest
`7ccecff73728ce361669061613573c2e7089357c533258d64eb97830791da57a`.

## Windows 11 USB preparation

1. Keep the spare fitted in the Dell and the original OS drive removed. Connect
   the Dell to mains power.
2. On Windows, open Rufus. Select the USB stick under **Device** and select
   `C:\Sources\FortressOS\bin\fortress-installer.img` under **SELECT**.
   If the picker hides it, change the file filter to show all files.
3. Click **START** and allow Rufus to overwrite that USB stick. This is a raw
   `.img`, which Rufus writes in DD mode. Preserve its partition table; do not
   extract files or use the inspection/preview ISO. If Windows subsequently
   offers to format the stick, cancel that prompt and safely eject it.

The raw GPT source is required for boot-source provenance. See the
[Rufus FAQ](https://github.com/pbatard/rufus/wiki/FAQ) for raw `.img` support.
Image SHA-256:
`ca0dca21ce6adc2d3741a1ec6165dc5ef186cdeb9bc8973c3a24a670da6cad83`.
Build and gate receipt: `bin/fortress-installer-dell5590-manifest.json`.

## Installation and first boot

4. Insert the USB in the Dell. Power on and tap **F12**; select the UEFI USB
   entry, then the FortressOS installer entry.
5. Check that disk **1** displays `PC401 NVMe SK hynix 256GB` and approximately
   **256 GB**. Type `1` and press Enter. The screen announces the installer-file
   check before it starts; wait for the username prompt.
6. Choose a lowercase username (for example `fabio`), then a login password of **8–128 printable ASCII
   characters** and confirm it. Choose a **different** recovery passphrase and
   confirm it. Passwords appear as asterisks. A short password, mismatch or reused
   recovery password retries that step and keeps your disk selection.
7. Review the selected disk and username. Type **YES** and press Enter to erase
   the spare and install. **NO** or Escape cancels. A mistyped confirmation keeps
   your answers and asks again; it does not erase anything. Keep power and USB
   connected. The screen reports copying, account creation and verification.
   The layout remains 1 MiB BIOS, 64 MiB boot and 256 MiB journaled root; the
   remaining space is unallocated.
8. Photograph completion, power off, remove the USB and boot the spare. If the
   new entry is absent, press **F2**, open **Boot Sequence → Add Boot Option**,
   select the spare's `\EFI\BOOT\BOOTX64.EFI`, name it `FortressOS`, apply/save,
   then select FortressOS through F12.
9. Log in with your chosen username using the password chosen during installation. Run:

```sh
id
sudo id
echo dell5590-first-install > install-check.txt
sync
sudo shutdown
```

`sudo` uses your login password. Restart without the USB, log in again and run
`cat install-check.txt`; expect `dell5590-first-install`. Send a photograph of
the completion screen and this successful boot/persistence check. Keep passwords
out of photos and messages. Recovery and independent Linux audits follow this
first acceptance checkpoint.

If the target is unavailable or installation reports a failure, photograph the
message and leave it for diagnosis. A failure is not installation success.

Dell documents [F12 boot selection](https://www.dell.com/support/manuals/en-uk/latitude-15-5590-laptop/latitude_5590_om/one-time-boot-menu?guid=guid-71da763a-3f2f-4021-8ef7-3e50da009c74&lang=en-us)
and [F2 setup access](https://www.dell.com/support/manuals/en-ca/latitude-15-5590-laptop/latitude_5590_om/accessing-system-setup?guid=guid-fc869170-7119-4ace-9c13-5ac2d08ebaca&lang=en-us).
