# NVMe disk-selection installer

This separate installer profile implements the requested disk list. It shows a
numbered model-and-capacity line for each detected supported NVMe controller;
serial numbers, PCI addresses, namespace numbers and identity digests stay out
of the menu. No disk is preselected. Enter its number and press Enter.

The final confirmation repeats the selected disk number, model and capacity.
Only the selected disk number proceeds (for example `2` for disk 2). Blank input,
`YES`, and another number retry without losing answers. Escape returns to the
recovery step. Ctrl-C, `CANCEL`, or `NO` cancels. Username selection, eight-character minimum login/recovery passwords,
masked secrets and a byte-driven installation progress bar remain available.
Password entry and confirmation start hidden. Tab toggles visible/hidden input
on the local framebuffer only, including characters already entered. Backspace
works in either mode. Local frames redraw to remove wrapped visible text, and
Enter/Escape/Ctrl-C clear them before restoring the original output route.
Without a local screen, Tab leaves input masked. No plaintext is sent to UART
or provisioning logs.
Both passwords receive a Weak / OK / Strong hint after entry. Weak uncommon
passwords remain accepted; a small local common-password list rejects familiar
words/patterns, case variants and numeric/!/? suffixes. The eight-character
minimum remains, and no capitals/digits/symbols are required. Length/diversity
hints are advisory, not entropy estimates or a breach-database check.
After confirming the recovery password, type `SHOW` to reveal it on the local
screen only, then Enter to clear it. Serial output never receives the plaintext.
Type `YES` to acknowledge it is stored safely; other answers retry and Escape
returns to the account step; Ctrl-C cancels before disk writes. The existing
full password confirmation remains; no extra partial-password exercise is required.
Every screen uses the same FortressOS installer header and step number,
including file checks, password visibility, hashing waits, installation progress
and results. Password redraws retain account/recovery context and distinguish
entry from confirmation. The five screens cover keyboard layout (US QWERTY or Belgian AZERTY), disk,
login account and computer name, independent recovery password and final confirmation. Existing
partition summaries appear at confirmation, rather than cluttering the disk list.
They use simple labels such as Windows-type and EFI boot; partition metadata
does not establish an installed OS, filesystem type or used space. Uncertain
contents say "Contents unknown - this disk may contain data." No partition table
does not imply an empty disk. The final warning says "Installing will erase this disk."
Errors explain the failed operation and give next steps, with signed technical
stage/error/call details underneath. File-check or acquisition failures say no
installation data was written. Deployment failures warn the disk may be partly
erased and must not be booted yet. Error screens wait for Enter before returning.
The messages distinguish syscall return codes from raw backend errors; they do
not claim to identify a failed physical component from a generic I/O result.
Completion waits for Enter before restarting; the installed system keeps an
installation log at `/var/log/fortress-install.log` without passwords or hashes.
The USB copy is `/fortress-install.log` at the root of the separate FAT16
`FORTRESSLOG` volume. This volume appears alongside the boot volume when
reading the USB on another computer; its drive letter may differ. Boot files
and installer payloads stay protected. The log records stages, selected disk
model/capacity, username, completion, cancellation and signed failure details.
Recent attempts are retained during installer retries; the bounded 4 KiB file
keeps its newest entries. Laptop logging requires a provisioned root filesystem;
early failures can only be saved on the USB. ISO/read-only/unsupported USB
logging shows a warning and installation can continue. Filesystem taint or
failed durability barriers still prevent a successful installation.

USB logging rejection/failure reasons are retained in the RAM session log and
copied into the installed `/var/log/fortress-install.log`, even when the USB
file cannot be written. Messages distinguish absent transport, unknown/read-only
durability, incompatible configuration/geometry, source/GPT mismatch, changed
FAT metadata, validation flush, data write, flush, read-back failure and byte
mismatch. Data I/O failures include the sector. The first failure disables
further USB log I/O for that boot; installation continues when otherwise healthy.
The block callbacks return booleans, so these messages identify the failed
operation without inventing a SCSI sense code or hardware cause. Rufus metadata
changes are a possible diagnosis, not established by a generic USB warning.

New installations make `/var` and its template subdirectories root-owned `0700`;
template files under it and `/var/log/fortress-install.log` are root-owned `0600`.
Ordinary users cannot list `/var`, traverse into it, or read/write/delete its log.
Root can access it, including with `sudo`. Both installer log refreshes verify
ownership and `0600`. Earlier installed roots remain bootable with their original
`0755` `/var`; updating the installer image alone does not change that installation.
The USB FAT log has no Unix owner/mode enforcement and remains independently readable.

The account screen asks for computer name first, then username, then login
password, keeping the username and password prompts together. The computer
name defaults to `fortress`. Names
contain 1-63 letters, digits or hyphens, with a letter/digit at both ends.
Invalid names retry in place; the chosen name survives back navigation and is
saved to `/etc/hostname`. The shell reads the effective user's account name and
this file for a prompt such as `fabio@dell-5590:/home/fabio$ `. After `cd /tmp`,
it shows `fabio@dell-5590:/tmp$ `. Root uses `#`; long paths keep their tail
visible without hiding the user and machine name. Invalid/missing hostnames
fall back to `fortress`.

Before disk writes, Escape moves to the previous numbered screen (on step 1,
it stays there). Ctrl-C cancels at any input prompt; CANCEL also cancels at
non-password prompts. Going back retains the chosen disk, keyboard layout,
username and completed password hashes. Enter keeps a saved choice; CHANGE
replaces a saved password. Partial password input is wiped when leaving its
screen. Changing the login password invalidates recovery and requires a new,
independent recovery password. Once deployment begins, no navigation reader
runs; the existing bounded write/failure path owns the operation.

## Separate image and build

The raw image is `bin/fortress-installer-multi.img` (553648128 bytes, 528 MiB).
SHA-256: `6032f400a2031291c6a8588990b0d5fc83ba063a021a7c7d59fed4085bf40ef2`.
The audited producer workspace is `build/root-phase4/media-j2a27xfq`.
Flash that raw IMG to the installer USB using the same Rufus/DD procedure as the
previous Dell image. Boot its installer entry using F12. Image extraction to a
new USB filesystem is not supported: the source GPT identity must be preserved.

This image permits an explicit user-selected NVMe target rather than requiring
the previous Dell spare's compiled fingerprint. Keep the existing installed
system and drive-bound media if they are still needed. The old Dell hardware
acceptance applies to the previous image, not this revision.

Build in a fresh isolated checkout with:

```sh
make -j4 INSTALLER_MULTI=1 bin/fortress.elf bin/initramfs.tar build/tool-install.elf
python3 scripts/create_installer_media.py --multi \
  --kernel PATH/bin/fortress.elf --bootstrap PATH/bin/initramfs.tar \
  --installer PATH/build/tool-install.elf
```

Do not mix profile flags or cached objects from another profile. Normal live,
disposable, inspection and fingerprint-bound registration remain separate.

## Storage contract

- At most eight NVMe controllers are pinned from a complete boot inventory.
  Each controller owns separate queue pages, bounce storage, indices and fatal /
  DMA-quarantine state. Failed initialization is never retried or recycled.
- The current installer accepts one conventional 512-byte active namespace per
  controller. Unsupported or incomplete inventories fail closed. SATA, USB data
  disks and multiple namespaces per controller are outside this profile.
- Inventory/identity checks remain mandatory before target operations. The
  selected full identity and index are checked again at lease acquisition.
  Enumeration cannot switch controllers while a lease exists.
- The installer source disk GUID remains excluded. An NVMe containing that
  source GUID is listed as unavailable. USB installer media is excluded by the
  transport policy. Unknown/corrupt boundary metadata is not repaired or erased
  during discovery. Valid existing MBR and unrelated GPT layouts are selectable;
  sector-read failures reject discovery, and matching primary or backup source
  GUIDs remain excluded even when source metadata is damaged.
- Whole-disk confirmation replaces the selected disk's partitions. Layout uses
  1 MiB BIOS, 64 MiB ESP and a journaled root partition extending to the last
  usable GPT sector. The filesystem expands to use that partition, with only
  sector/block alignment and an exceptionally short final group's metadata
  padding unused. Root filesystems above 1 TiB are rejected before target writes.
  This is not a secure erase of every sector.
- Installed boot scans GPTs read-only across the pinned controllers, requires
  exactly one matching root PARTUUID with coherent GPT/type, then selects that
  controller before writable publication. Duplicate IDs enter authenticated
  recovery without root writes. Readiness, filesystem UUID, account and
  executable admission remain mandatory.

## Verification

Plain-error revision: actual CLI ASan/UBSan PASS in `cli-60h26cfi`, including
validation and acquisition failures before writes, verification failure after
deployment, unknown-code fallback, memory/space guidance and original signed
syscall/backend details. Actual NVMe timeout/failure screens pass under BIOS
and UEFI in `confirmation-o_u3in3a`; framebuffer output visually checked.
The retained timeout manifest has the earlier generic confirmation scope label;
its case records identify the actual timeout runs. Both source and target stay
hash-identical under the throttled fixture. This tests bounded failure handling,
not a physical disk fault or a full installation rerun.
Four BIOS/UEFI x disk-choice UI flows also PASS in `confirmation-8krmtjt5`,
covering password visibility, recovery, headers, navigation, confirmation and
cancellation with unchanged target/non-target/source media hashes.


Consistent-header revision: actual CLI ASan/UBSan PASS in `cli-p94nnf9p`,
including password/recovery redraw context, confirmation labels, installation
and success/failure headers, while retaining routing/wipe/navigation checks.
BIOS/UEFI x both disk choices PASS in `confirmation-p0m14qt6`, including all
five headers, file-check context, password visibility/confirmation, recovery
storage, navigation and cancellation. Local confirmation framebuffer visually
checked. Disks/source media remain unchanged; focused cases stop before writes.
The historical full installation matrix below was not rerun.


Password-visibility revision: actual CLI ASan/UBSan PASS in `cli-bpxc2zrz`.
Cases cover Tab before/after input, repeated show/hide, visible backspace,
confirmation of both passwords, Escape while visible, unavailable-screen
fallback, 128-character input, routing restoration and no UART plaintext.
BIOS/UEFI x both disk choices PASS in `confirmation-06xblrw8`: Tab reveal,
visible typing/backspace, Tab hide and Enter for both password fields and their
confirmations. Framebuffer capture visually checked. UART transcripts contain
no passwords, and target/non-target/media hashes remain unchanged. These focused
cases cancel before installation; the historical full installation matrix below
was not rerun.


Back-navigation revision: CLI ASan/UBSan passes in `cli-h_3evf2j`.
Actual-code adapter cases traverse every previous screen, retain password hashes
without rehashing, switch disks, replace login/recovery passwords, shorten the
username without leftover characters, and cancel with partial secret input.
The unchanged raw-write and lease gates remain outside this UI change.
BIOS/UEFI x both disk choices PASS in `confirmation-n48b433g`: Escape through
all previous screens, Enter to retain answers, fresh confirmation, cancellation,
local-only recovery reveal and unchanged disk/media hashes. These focused cases
stop before installation; the historical full install/boot/recovery matrix was
not rerun. `confirmation-2jd8he0z` retains an interrupted runner attempt that
still waited for the prior prompt wording; its QEMU was stopped via QMP before
the corrected run.


Recovery-storage revision: host ASan/UBSan passes in `cli-y4m1fmt_`, including
repeated reveals, cancellation while visible, routing restoration, wiped
requests and refusal to reveal without a local screen. BIOS/UEFI x both disk
choices passes in `confirmation-5ng8vtcc`: SHOW displays locally, Enter hides it,
NO retries the storage question, YES proceeds, and serial transcripts contain
no plaintext. Captured framebuffer output was visually checked. Target,
non-target and source media hashes remain unchanged; these cases cancel before
installation. The full install/boot/recovery matrix below is historical.

Password-guidance revision: host ASan/UBSan passes in `cli-xchrjiki`;
BIOS/UEFI x both disk choices passes in `confirmation-x6pi797q`. The guest
checks reject common login/recovery passwords, accept an uncommon eight-character
Weak password, and show OK and Strong hints for lowercase passphrases. Passwords
stay masked and cancellation leaves disks/media unchanged. These focused cases
stop before disk writes; they do not repeat the historical full installation gate.

Previous revision full BIOS/UEFI gate: `build/root-phase4/multi-_43ow6a3/manifest.json` — PASS.
The number-confirmation revision passes the CLI ASan/UBSan gate in
`build/root-phase4/cli-21u0gcln` and four BIOS/UEFI disk-1/disk-2 confirmation
checks in `build/root-phase4/confirmation-fxtgrga9`. Blank, YES and wrong-number
inputs retry; cancellation preserves both disks and source media. These focused
guest cases stop before installation; full installation was tested on the prior revision.
Updated failure-screen/deadline checks also pass under BIOS and UEFI in
`build/root-phase4/multi-timeout-tcd_reox`. Host adapter evidence and interrupted
runs are recorded in the [Phase 4 evidence](roadmap/persistent-root-phase4.md).

`scripts/test_installer_multi.py` uses disposable regular files only. It checks
BIOS and UEFI cancellation, selection of each drive over existing MBR/GPT layouts, unchanged unselected/media
hashes, independent Linux filesystem/account audits, three boots with both
drives present, duplicate root IDs and installer-source exclusion. UEFI USB
uses EHCI because of the existing QEMU/OVMF xHCI preboot fixture limitation.

Run against retained audited media with:

```sh
FORTRESS_INSTALL_ACCEL=kvm make test-root-phase4-multi \
  INSTALLER_MEDIA=build/root-phase4/media-j2a27xfq
```

Physical two-NVMe acceptance is pending user testing. Agent commands never open
or write a physical disk. The user flashes the USB and chooses the physical
target in the installer.

The logging revision passes actual-code sanitizer gates and focused BIOS/UEFI
installation, three-boot persistence, authenticated recovery, read-only USB,
cancellation and deadline-failure gates. Retained evidence and the UEFI EHCI
boot/xHCI logging-clone qualification are in the
[Phase 4 evidence](roadmap/persistent-root-phase4.md). Dell USB logging still
needs physical acceptance. Reproduce the focused gates with:

```sh
make test-install-log-host INSTALLER_MEDIA=build/root-phase4/media-j2a27xfq
FORTRESS_INSTALL_ACCEL=kvm make test-install-log \
  INSTALLER_MEDIA=build/root-phase4/media-j2a27xfq
```
