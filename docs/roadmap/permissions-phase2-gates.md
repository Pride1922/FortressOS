# Permissions Phase 2 enforcement delivery

2026-10-10. Phase 2 complete locally. Production kernel CFLAGS enable
`FORTRESS_DAC_ENFORCED`. No GitHub changes or Dell/physical testing.
All pre-existing uncommitted work was preserved. Baselines of the initial 69
paths and this continuation's 77 paths, exact bytes and tracked diffs, remain
in `build/permissions-phase2/baseline` and `resume-baseline`. TCP commit
`056a12d` is the base; this continuation does not edit networking source.

## Enforcement gates resolved

1. EXT2, journaled EXT4 and runfs actor callbacks reread authoritative metadata,
   authorize and perform lookup/create/open/truncate/unlink/rename/setattr under
   their filesystem lock. Namespace decisions check the current named victim,
   parent and destination before mutation allocation, disk writes or journal
   staging. Filesystem locks never nest with the process lock. Unknown mutable
   adapters fail closed. TarFS/devfs use immutable metadata, with read-only,
   NODEV and raw-block DAC plus CAP_SYS_RAWIO policy. The nonjournal EXT4 write
   workbench remains trusted-only; actor mutation there returns EOPNOTSUPP.
2. Original dot components survive syscall path construction. Every directory
   requires search, including components followed by `..`; missing components
   cannot disappear. File `.`/`..` and trailing slashes reject non-directories.
   Mutable filesystem callbacks validate dot navigation while holding owned
   references; mount-root `..` reaches the mount's VFS parent. CHDIR/spawn cwd
   canonicalization happens only after a successful full walk. Cwd remains a
   validated string under the existing architecture: a later directory rename
   can make that string unavailable; this phase adds no inode-backed cwd.
3. Same-identity rename is authorized under filesystem exclusion, including
   sticky and read-only checks. Runfs directory moves update bounded descendant
   paths under its lock. Existing filesystem restrictions remain: EXT2 rejects
   active-open unlink/replacement; EXT4 directory rename/replacement retains
   its supported-operation limits.
4. Creation derives owner, umask and parent-setgid before mutation allocation.
   Metadata setters and required set-ID stripping use fresh inode values.
   Admitted descriptors retain read/write/readdir rights after chmod. Positive
   writes and admitted truncation clear required bits unless CAP_FSETID;
   zero writes preserve them. Journaled EXT4 commits these mode changes with
   content/identity in the same transaction. EXT2 remains nonjournaled: uncertain
   I/O taints the filesystem and does not gain crash atomicity from this work.
5. Actual-filesystem denial tests compare complete durable/pending image bytes
   (including allocation maps/counters and namespace), write/flush events and
   EXT4 staging state; runfs compares its complete bounded pool. Controlled
   pthread barriers change a parent's mode or replace a named victim after
   lookup and before authoritative admission. Denial preserves the post-change
   state. These are host adapter proofs within finite coverage, not real IRQ
   or scheduler proofs.
6. Syscalls 58–64 implement umask/chmod/fchmod/chown/getresuid/getresgid/getgroups.
   Stat's 40-byte v1 ABI remains at 57. Chown has explicit keep flags and full
   32-bit IDs; credential getters validate every output before publication;
   `UMASK_QUERY` reads without temporarily changing the mask. Numeric chmod,
   chown, id, ls -l and parent-shell umask are delivered. Capability enforcement
   covers reboot and low-port bind; signal decisions use current bound actor
   and target values under the process lock, including staged/group/signal-0
   paths. Debug syscall 65 only drops its own test process to UID/GID 1001,
   zero groups/caps; it is absent from normal kernels.

## Final evidence

- `build/permissions-phase2/final-regression.log`: aggregate `make test-host`,
  generated permission matrix, syscall, runfs and privilege targets PASS.
  Independent Python oracle: 1,376,256 DAC cases, 786,432 creation cases,
  7,168 umask/bounds vectors and 131,072 chmod proposals, plus sticky, metadata,
  content-mode, device and hostile-value cases under ASan/UBSan.
- `make test-perm-runfs-host` also passes the final SGID-parent, immutable device,
  raw-I/O capability and NODEV additions. Filesystem tests cover full-width
  owners, held descriptors, set-ID publication, original-component walks,
  parent navigation, no-op rename and SGID-parent file/directory creation.
- `build/permissions-phase2/filesystems-final-verified.log` and
  `build/permissions-phase2/enforcement-8sha720d`: final actual EXT2 128/256-byte
  inode and journaled EXT4 admission/interleaving tests, Linux fsck/stat audits,
  and 4,088 metadata/content crash cuts. Chmod and chown each have 77 events,
  write 86 and truncate 271, cut before/after every event across four atomic
  persistence models (flush-only, immediate, alternating odd/even writes).
  Recovery checks complete old-or-new mode, IDs, size and bytes; eight retained
  representative images receive independent Linux audits. No sector-tear or
  physical-device claim follows. The manifest captures source hashes before
  compilation and checks them again after completion.
- Phase 0 creation regression: `build/permissions-phase0/inodes-l920f8r4`,
  1,000 attribute-creation cuts plus all 4096 modes/full-width owner tests and
  Linux audits PASS. These are separate from the new four-operation inventory.
- Final test snapshot `guest-workspace-4vctvd8s` under
  `.codex-remote-attachments/ext4-phase9`: build and `build-phase2-final-guest.log`
  PASS; `build/permissions-phase2/guest-as8axqzf` retains 4/4 BIOS/UEFI × SMP=1/4
  non-root Ring 3 DAC/metadata/capability and root tool/shell results, kernel/
  probe hashes, UART logs and exact guarded QEMU argv. Test images explicitly
  compile TEST_PERMISSIONS_ENFORCEMENT. Only disposable USB journal fixtures,
  ISO and paired OVMF are attached. No cross-core socket claim.
- Normal production snapshot `guest-workspace-o4xj4uys`: kernel/ISO build and
  BIOS/UEFI shell regression 2/2 PASS; `build-phase2-normal-symbols.txt` confirms
  test credential and interleaving hooks are absent. Snapshot manifests retain
  source hashes and explicit updates. No production disk image was rebuilt.
- Final source inventory: 173 classified sites, zero unclassified calls,
  `build/permissions-phase1/audit.json`. `git diff --check` passes.

Earlier failed attempts and superseded campaigns remain available; they are
not final acceptance evidence. The final manifest ties this delivery to sources,
HEAD and artifacts. No general credential transition API, login, sudo or set-ID
exec is added; those remain Phases 3–4. Shared user threads require a separate
shared credential-owner protocol before introduction.

Normal journaled `bin/fortress.img` is unchanged, SHA-256
`d130bec6d0b396a968135fb2132b9543a8774c8889b24fce464f88a10cd68c28`.
All changes remain local and uncommitted.
