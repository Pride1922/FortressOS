# SYS_FSTAT: retained descriptor metadata

Implemented and verified 2026-10-17. SSH Phase 3 is not started. No commit or push.

## Contract and ABI decision

Syscall 78 takes RDI=fd and RSI=writable output, returning zero on success.
The user approved reusing the existing 40-byte `stat_ext_v1_t` because legacy
16-byte `vfs_stat_t` lacks uid/gid. Output contains size=40, version=1, 64-bit
file_size, type, mode, uid, gid, mnt_flags and zero reserved. Neither existing
layout has timestamps, and none were added. SYS_STAT, SYS_STAT_EXT and both
struct layouts remain unchanged. Full-register out-of-range fd values are rejected.

All types are accepted and reported as published by VFS. Device/pipe distinctions
are represented by existing mode/type values; no new socket or device behavior.
Errors are EBADF (caller fd missing/closed/out of range), then EFAULT (entire
40-byte output not writable), then EIO (reference retention or metadata failure).
A stack-local snapshot is copied only on success; failures leave output untouched.

The handler borrows the caller's own fd entry, validates output, takes an existing
file reference, calls `vfs_metadata(file->node)`, copies and releases the temporary
reference. It performs no path lookup, allocation, offset change or new locking.
Existing backend metadata callbacks retain their own coherent snapshots and may
perform bounded filesystem I/O. There is no new fd-table lock: the existing
own-table borrowed-reference contract applies before any wait/signal boundary.

Renaming or replacing a pathname cannot redirect this descriptor metadata lookup.
Unlink preserves the retained object where the existing backend supports open
unlink. Tests use actual runfs; legacy ext2 still rejects unlink of open objects.
This adds neither universal POSIX unlink support nor new backend lifetime rules.
The metadata is current at snapshot, not frozen at open. Host-key publication,
ancestor validation and durable storage design remain separate Phase 3 work.

## Verification and artifacts

- `make test-fstat-host`: actual syscall/VFS/runfs under ASan/UBSan, regular,
  directory and stream metadata, full-width fields, output guards, no handler
  allocation, offset/reference preservation, error precedence, callback failure
  after partial internal fill, invalid pointers through the hosted validator,
  rename/replacement/unlink/duplicate retention and final reclamation: PASS.
- `make test-perm-syscalls-host` and `make test-pipe-host`: PASS.
- `make test-fstat`: isolated disk-free BIOS/UEFI × SMP=1/4/8, 6/6 PASS.
  Each boot checks `/etc/motd` and `/bin/hello` against test archive sizes/modes
  and root ownership, directory type, missing-file open failure, actual Ring 3
  null/kernel/overflow/read-only-code pointer rejection and descriptor errors.
  One direct and eight ordinary unpinned child selftests per boot prove retained
  rename/replacement/unlink/dup metadata and path SYS_STAT/SYS_STAT_EXT behavior;
  pipe metadata and actual `ls -l /etc/motd` output pass. Total 54 selftests.
  Adjacent CPUID observations show multiple CPU IDs in every SMP=4/8 case;
  these demonstrate worker placement, not an exact syscall-entry CPU trace.
- Final `make -j8 all`: PASS, including raw-image structural/fsck checks.
  Release initramfs lacks fstat-probe and crypto-probe; its exact bytes occur in
  the release ISO and raw image, both independently scanned for probe exclusion.
- Baseline hashes prove VFS/public header, runfs, scheduler, permission syscall,
  time, accounts and sampled network sources unchanged. Existing SYS_STAT and
  SYS_STAT_EXT handler bodies are byte-identical to HEAD. No entry ABI change.

Evidence retained under `.codex-remote-attachments/fstat/`: `host.log`,
`permissions.log`, `pipe-regression.log`, `campaign.log`, `serial-audit.json`,
`release-build.log`, `release.json`, `before.json` and `preservation.json`.
The full guest campaign is
`.codex-remote-attachments/fstat-qemu-20261017-074234/`: `summary.json`,
`manifest.json`, `build.log`, isolated workspace and each case's `argv.json`,
`serial.log`, `qemu.stderr`, `result.json` and private UEFI vars where applicable.
The manifest verifies original source/release media preservation during the
isolated campaign; the normal release rebuild occurred afterwards.

The runner creates only optical and copied pflash drives, explicitly rejects
other drive arguments, uses no data disks/networking, keeps UART drained, and
bounds waits with process cleanup/reaping. The test probe is enabled solely with
`FSTAT_TEST_PROBE=1`; default release staging removes any stale probe file.
Host pointer adapters do not prove page-table enforcement; guest cases exercise
real validation. No physical-hardware acceptance or storage durability claim.
