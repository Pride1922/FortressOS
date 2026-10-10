# Permissions Phase 5 — Dell Latitude 5590 acceptance

Status: **PENDING USER RESULTS**, 2026-10-10. All listed outcomes below are
expected results, not physical observations. The agent has performed no
flashing, physical disk access or Dell operation.

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
