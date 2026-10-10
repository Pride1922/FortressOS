# Permissions Phase 5 — Dell Latitude 5590 acceptance

Status: **USER PHYSICAL FUNCTIONAL AND LINUX AUDITS PASS WITH LIMITS**,
2026-10-10. Observations below distinguish supplied evidence from the checklist's
expected results. The agent has performed no
flashing, physical disk access or Dell operation.

User observations received 2026-10-10 (image identity not yet confirmed):

- Root login with a password rejected: user-reported PASS.
- Operator passwordless login succeeds: user-reported PASS.
- Passwordless/temporary-home warning not visible: user-reported FAIL;
  diagnosis pending. Remaining checks and overall physical acceptance pending.
- User reports the complete access/sudo command sequence PASS: plain
  `cat /etc/shadow` and `/bin/dmesg` denied; `sudo dmesg` succeeds;
  two `sudo id` invocations succeed with the expected warnings/root identity;
  parent `id` remains operator; `sudo /bin/sh-builtin false` followed by
  `echo $?` returns 1. This is user-reported physical evidence, not an
  independently captured transcript. USB nosuid, ownership/reboot and Linux
  integrity audits remain pending.
- Subsequent user photo shows `sudo /bin/shell`, root `id`, directory
  preparation, `exit`, then `id` still reporting UID/GID 0. Return to operator
  is unresolved and overrides any assumption that this nested-shell step
  passed. The photo alone does not establish which shell remains active or
  whether the booted image matches the delivered hash. Pause operator-based
  ownership/nosuid checks pending process/image diagnosis.
- Follow-up photo shows login PID 94 -> operator shell 95 -> sudo 113 ->
  elevated shell 114 -> ps 121. Elevated environment is HOME=/root,
  USER/LOGNAME=root. A subsequent `exit` returns to UID/GID 1000 with groups
  1000,10,44,104: return-to-operator PASS supported by photo. The first
  ineffective `exit` remains unexplained; this evidence does not demonstrate
  parent credential elevation. Operator-based checks may resume; image
  identity and login-warning visibility remain unresolved.
- Ownership/DAC photo confirms owned.txt initially operator:operator,
  mode 0640, size 25; sudo chown changes IDs to 12345678:87654321 while
  retaining mode 0640. Plain cat fails with exit status 1; sudo cat displays
  phase5-owned-persistence. Passwordless sudo warnings are visible for both
  chown and cat. This live ownership/DAC step PASS; reboot persistence and
  independent Linux byte/integrity verification remain pending.
- First USB nosuid photo: copy/chown succeeded (root:root, size 71608), but
  command was mistyped as `sudo chmox 4755 ...` and failed to start. Copy
  remained non-executable; launch reports Unable to load executable, parent
  remains UID/GID 1000. This attempt is not nosuid acceptance; retry actual
  chmod 4755 and verify mode before execution. Retain failed attempt photo.
- Corrected USB nosuid photo confirms `sudo chmod 4755` succeeds and ls
  displays -rwsr-xr-x root:root, size 71608. Executing the USB copy prints
  `sudo: privileged installation required`, exit status 1; final id remains
  UID/GID 1000, groups 1000,10,44,104. Live USB nosuid test PASS supported
  by photo. Clean reboot persistence and independent Linux audits pending.
- Reboot login photo clearly displays `WARNING: live-media operator login
  is passwordless; home is temporary.` before the first login prompt.
  Warning visibility PASS supported by photo; supersedes the earlier
  not-observed report. Post-reboot ownership/bytes/nosuid checks pending.
- Post-reboot photo confirms operator UID/GID 1000, groups 1000,10,44,104,
  HOME=/run/user/1000 and USER/LOGNAME=operator. Temporary file cannot be
  opened (pre-reboot creation has not been independently captured). Persistent
  owned.txt retains 12345678:87654321, mode 0640, size 25; plain cat is denied
  with status 1 and sudo cat displays the original text. USB sudo retains
  root:root 4755, size 71608, rejects privileged installation with status 1,
  and parent remains operator. Reboot ownership/DAC/displayed-content/nosuid
  PASS supported by photo. History auto-save warning matches the documented
  existing history-path limitation. Independent Linux integrity/exact-byte
  audits and delivered-image identity confirmation remain pending.
- User supplied PowerShell Get-FileHash output for bin/fortress.img matching
  7c6a70d1a50252efed9b5f07e44c5cb210ae94f5ecd7fdcea20a60951c03b53f:
  source-image identity PASS. This verifies the source file, not a USB
  read-back or byte identity of the booted media. Linux audits pending.
- Linux identification photo: /dev/sda has TRAN=usb, model SanDisk SSD PLUS
  240GB, reported size 115.1G; /dev/sda2 is 64M EXT4 with expected PARTUUID
  705536e3-a337-4b30-a0cf-0f316ebda29c. Mounted at
  /media/pride1922/FORTRESS_DATA with rw,nosuid,nodev,relatime,errors=remount-ro.
  Selected USB partition identity supported by photo. Linux automounted RW,
  so subsequent clean-state audit is after that mount and cannot alone prove
  the untouched FortressOS shutdown state. Unmount before fsck; no repair.
- Linux fsck photo confirms successful umount /dev/sda2 and empty findmnt
  result; e2fsck 1.47.0 -fn completes all five passes, 19/512 files,
  1100/16384 blocks, exit 0. Independent post-Linux-mount filesystem
  consistency PASS; no repair performed by this command. Linux ownership,
  journal information and exact-byte/hash checks remain pending.
- Linux dumpe2fs photo confirms filesystem state clean, has_journal,
  extent and metadata_csum features, 4096-byte blocks, 256-byte inodes,
  journal inode 8, journal_checksum_v3, journal start 0 and 4096k journal.
  Independent journal/profile metadata check PASS after the disclosed Linux
  RW mount. Inode ownership and exact-byte/hash checks remain pending.
- Two Linux debugfs photos independently confirm inode 18 owned.txt:
  regular file, mode 0640, UID 12345678, GID 87654321, size 25; inode 19
  nosuid-sudo: regular file, mode 04755, UID/GID 0, size 71608. Independent
  on-disk ownership/mode/size audit PASS. Mounted numeric ownership and
  exact-byte/hash comparisons remain pending.
- Final Linux photo confirms corrected `mount -o ro,noload`, numeric ls
  ownership/modes/sizes, both expected SHA-256 hashes and exact owned.txt
  bytes ending 0a. Unmount completes without diagnostic. An initial mount
  command typo reports bad usage and an accidental nload command is not
  found; corrected audit succeeds. Mounted ownership and exact-byte audit
  PASS supported by photo.

Physical closeout: supplied Dell photos/user reports support login/root
rejection, visible default warnings, sudo and parent credentials, high-ID
ownership/DAC across reboot, USB nosuid and independent Linux integrity,
journal metadata, ownership and exact bytes. Functional checklist accepted
with these retained limits: no pre-write USB read-back hash or full booted
kernel identity evidence; firmware/CPU count and internal-NVMe exclusion
not independently captured; Linux automounted RW before the integrity audit;
temporary-file creation before reboot not captured; first ineffective exit
remains unexplained (later exit correctly restored operator). No physical
crash/tear, general hardware or alternate-firmware acceptance is claimed.

## Verified image and test boundary

Use the fresh `bin/fortress.img`, also retained as
`build/permissions-phase5/delivery/fortress-permissions-phase5.img`.
Size: 136,314,880 bytes (130 MiB). SHA-256:

```
7c6a70d1a50252efed9b5f07e44c5cb210ae94f5ecd7fdcea20a60951c03b53f
```

Data partition PARTUUID: `705536e3-a337-4b30-a0cf-0f316ebda29c`.
The image contains the current production kernel/initramfs, journaled EXT4,
locked root, passwordless operator with warning, temporary HOME=/run/user/1000,
and passwordless sudo with its warning. No login bypass, fixed credential
test syscall, probe binary or authentication cache is installed. Optional
explicit operator build hashes remain supported; this delivered image uses
the approved passwordless defaults.

Use **one explicitly disposable USB stick** for boot and data. Identify its
current model, serial and capacity before imaging; prior device numbers or
earlier erase authorization are not identity evidence. You perform imaging
yourself. Preserve anything needed from that stick first. Keep all other
external data media disconnected. Never select the internal NVMe as a target.
Keep ENABLE_NVME_RAW_PATTERN_TESTS and every raw-write test gate disabled;
do not invoke physical EXT4 interruption/workbench tests for this checklist.
The normal kernel continues to exclude internal NVMe from mounting/test I/O.
The explicit USB PARTUUID and durability/GPT admission still govern /mnt.

Verify the source image with PowerShell:

```powershell
Get-FileHash -Algorithm SHA256 C:\Sources\FortressOS\bin\fortress.img
```

After your imaging operation, independently read back the first 130 MiB of
the **identified disposable USB only** and compare with the SHA-256 above.
Do this before FortressOS writes persistent test data. The complete disk hash
will legitimately change after filesystem writes; do not require it to match
the shipped hash after the test. Keep the original image immutable.

## A. Boot and login

1. Boot the selected USB on the 5590 (UEFI first; BIOS too if available).
   Record firmware mode, CPU count, media identity, mount/durability messages
   and a photo/UART log. The selected journaled EXT4 must mount at /mnt with
   the expected PARTUUID. No internal NVMe filesystem may mount or be tested.
   If it falls back to RO or mount admission fails, stop write tests and retain
   the diagnostics; do not weaken the gates.
2. At login, enter `root`, then any test input when asked for a password.
   Expect rejection and the normal failure delay. Do not enter a real password.
3. Enter `operator`. Expect immediate passwordless login and a visible warning
   that login is passwordless and the home is temporary.
4. Run `id`, `whoami`, `env`, `disk list`, and `ps`. Expect operator/UID 1000,
   GID 1000 (zero-capability login is verified by the automated probe),
   HOME=/run/user/1000, USER/LOGNAME=operator and PATH=/bin. Process listing is
   public. Check `ls -l /run/user` for the 1000 entry's operator ownership and mode 0700.
   Run `echo temporary-home > /run/user/1000/dell-home.txt`; read it back.
5. `cat /etc/shadow` must fail. `/bin/dmesg` must fail; `sudo dmesg` must work.
   Retain the actual diagnostics and exit status. Ctrl-C must recover a prompt.

The shell's existing history path is /mnt/.fortress/history, outside the
temporary HOME. A fresh root-owned /mnt can produce `history: auto-save failed`
under operator DAC. This is a known history-location limitation; do not change
USB ownership wholesale to hide it. It does not grant access or change HOME.

## B. Sudo and credential boundaries

Run `sudo id` twice. Each invocation must show its passwordless warning and
run as root; the ordinary shell remains operator when `id` is run afterward.
Run `sudo cat /etc/shadow` and confirm locked root plus the empty operator
hash; do not publish hashes from an optional password-protected build.
Run `sudo /bin/sh-builtin false`; expect status 1. Run plain
`cat /etc/shadow` again; it must remain denied.

For shell-only commands such as mkdir and shutdown, use `sudo /bin/shell`.
The child wrapper `/bin/sh-builtin` deliberately excludes these commands.
For an optional image built with your explicit operator hash, both login and
each sudo invocation must request authentication; a wrong password must fail,
correct authentication must succeed, and a second sudo must ask again. The
automated password matrix already covers this; it is optional on the delivered
passwordless physical image.

## C. Disposable EXT4 ownership and USB nosuid

Enter an elevated shell and prepare only the test directory:

```
sudo /bin/shell
id
mkdir /mnt/permissions-phase5
chown 1000:1000 /mnt/permissions-phase5
exit
id
echo phase5-owned-persistence > /mnt/permissions-phase5/owned.txt
chmod 0640 /mnt/permissions-phase5/owned.txt
ls -l /mnt/permissions-phase5/owned.txt
sudo chown 12345678:87654321 /mnt/permissions-phase5/owned.txt
ls -l /mnt/permissions-phase5/owned.txt
cat /mnt/permissions-phase5/owned.txt
sudo cat /mnt/permissions-phase5/owned.txt
cat /bin/sudo > /mnt/permissions-phase5/nosuid-sudo
sudo chown 0:0 /mnt/permissions-phase5/nosuid-sudo
sudo chmod 4755 /mnt/permissions-phase5/nosuid-sudo
ls -l /mnt/permissions-phase5/nosuid-sudo
/mnt/permissions-phase5/nosuid-sudo id
id
sync
sudo /bin/shell
shutdown
```

Expected: owned.txt starts as 1000:1000 mode 0640, then displays numeric
12345678:87654321. Plain cat becomes denied; sudo cat returns exactly
`phase5-owned-persistence` and newline. The USB copy displays root:root 4755
but rejects execution as a privileged sudo installation; it must not print
root identity. The operator parent remains UID 1000. Sync and orderly shutdown
must succeed. Stop and record any I/O, durability, permission or panic failure.

## D. Reboot persistence

Boot the same USB again and log in as operator. Recheck `id` and `env`;
the temporary home returns to the approved runtime state and dell-home.txt
must be absent. `ls -l` must retain owned.txt mode 0640 and IDs
12345678:87654321. Plain cat must remain denied and sudo cat must return exact
bytes. The root-owned 4755 USB copy must still fail to elevate. Run sync,
then `sudo /bin/shell` and `shutdown` again before removal.

## E. Independent Linux audit

Use Linux after clean shutdown. Identify the disposable USB again with:

```
lsblk -o NAME,PATH,TRAN,VENDOR,MODEL,SERIAL,SIZE,FSTYPE,UUID,PARTUUID,MOUNTPOINTS
```

Confirm the selected USB serial/model and the data PARTUUID above. Unmount
only that selected data partition if Linux automounted it. Do not run fsck on
a mounted filesystem or substitute any NVMe device. Set `phase5_part` to its
verified USB `/dev/disk/by-id/...-part2` path and `phase5_mount` to a new empty
mount directory. The following commands are manual, read-only audits:

```bash
sudo e2fsck -fn "$phase5_part"
sudo dumpe2fs -h "$phase5_part"
sudo debugfs -R 'stat /permissions-phase5/owned.txt' "$phase5_part"
sudo debugfs -R 'stat /permissions-phase5/nosuid-sudo' "$phase5_part"
sudo mount -o ro,noload "$phase5_part" "$phase5_mount"
ls -ln "$phase5_mount/permissions-phase5/"
stat -c '%u:%g %a %s %n' "$phase5_mount/permissions-phase5/owned.txt" "$phase5_mount/permissions-phase5/nosuid-sudo"
sha256sum "$phase5_mount/permissions-phase5/owned.txt" "$phase5_mount/permissions-phase5/nosuid-sudo"
od -An -tx1 "$phase5_mount/permissions-phase5/owned.txt"
sudo umount "$phase5_mount"
```

Expect fsck exit 0, clean journaled EXT4, directory 1000:1000,
owned.txt 12345678:87654321 mode 640 with exact newline-terminated text,
and nosuid-sudo 0:0 mode 4755. Compare owned.txt hash with
`printf 'phase5-owned-persistence\n' | sha256sum`; compare the copy's hash
with `tar -xOf build/permissions-phase5/delivery/embedded-initramfs.tar bin/sudo | sha256sum`.
Retain Linux command output and exit codes. Do not repair a failed image;
retain a full read-only image copy for diagnosis first.

## Result handoff

Send results for A–E, firmware/CPU count, USB identity, initial image/read-back
SHA-256, boot/login/sudo/nosuid observations, reboot output and Linux audits.
Include exact failing command, diagnostic/status, and logs/photos if anything
fails. Never send your password. Keep the disposable media/evidence until
diagnosis is complete. Physical acceptance remains pending until these
user-supplied observations are reviewed; partial success closes only the
corresponding items.
