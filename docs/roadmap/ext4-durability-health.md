# EH-3 durability, health and recovery headroom — 2026-10-20

Follow-up: the [explicit recovery-required flag and bounded caller contract](ext4-recovery-required.md)
adds a fifth health bit and separate descriptor-sync/latency review. The original
campaigns below retain their recorded scope and preservation qualifications.

Implementation with bounded automated verification. Acceptance results are listed below;
superseded/debug campaigns are not acceptance. No physical-write, sector-tear,
package readiness or release-image claim.

## Contract and audit

See the [API and allocation policy review](../plans/EXT4_DURABILITY_HEALTH_DESIGN.md).
Existing syscall numbers, structures and lock ranks remain unchanged. The four
additive SYS_MOUNTINFO flag bits expose taint, uncertain replacement namespace,
pending orphan cleanup and a saved close-cleanup error independently of nominal
RW policy. `disk info` displays these flags. The query requires no device I/O;
tainted allocation counts must not be used as recovered capacity.

| Path | Durability/error boundary |
| --- | --- |
| `e4_file_write_actor`, `e4_namespace_end` | Ordered-data/JBD2 commit/home checkpoint before success; staged failure aborts without writes; uncertain I/O taints. Only success advances the supplied write offset. |
| `e4_transaction_commit_ordered` | Invalidates clean metadata before commit; republishes descriptors/cache only after checkpoint succeeds. A late checkpoint failure can follow durable activation: no rollback inference. |
| `e4_jclose` | Final-close cleanup errors cannot cross the void callback. Saves the first error and fences new transactions with recovery-required EAGAIN. Orphan chain and bounded engine intent survive cached-node retirement. |
| `e4_journal_drain`, `ext4_sync` | Retry eligible cleanup then flush. Pinned deleted files can remain. Report a saved close error once after successful retry; retry failure retains it. Tainted mounts return EIO without further I/O. |
| `storage_sync_all`, `sys_sync` | Attempt installed root and external storage separately, return first backend error. EROFS with no writable mount is preserved. No clean marker or writer freeze. |
| `ext4_freeze_and_sync`, `storage_freeze_all`, `sys_reboot` | Freeze new writes before drain. Pins/errors prevent clean-success and power transition. Acknowledging a saved close error does not mark clean; an explicit later freeze retry can finish. |
| admitted journal mount/recovery | Replay and orphan recovery are audited before publication. Failure publishes no usable mount; admitted recovery may already have written a prefix. Diagnostics name the failing stage. Reboot is required after uncertain I/O. |

The syscall write limit remains 16 KiB; the mounted backend's 32 KiB limit is
separately exercised as a positive short write in host tests. Arbitrary in-place
overwrites remain non-atomic. The existing shell sync failure message is generic
and suggests USB/mount-mode checks; the returned syscall error and mount health
are authoritative. Descriptor file/directory sync APIs remain deferred after
review: synchronous mutation plus the coarse SYS_SYNC barrier covers this phase.

Userspace audit boundary: shell `sync`, hostname persistence, shell history and
ifup's persisted configuration check SYS_SYNC failures. `nano` save,
`boot-snapshot`'s final save notice and several dhcpc paths currently ignore that
final result. Watchtower and timeline logging also contain best-effort output;
these callers are not an end-to-end durable-save acceptance claim. The new mount
health and explicit SYS_SYNC result remain available independently. Their tool
success-message/volatile-/run policies are not changed here; globally flushing
an unrelated failing mount is not equivalent to per-file failure attribution.
No TLS/HTTPS caller was changed as part of this audit.

Installed roots with initialized account service now enforce an ordinary-writer
floor of 256 KiB and 16 inodes against every staged allocation. This includes
logging, directory growth and extent metadata. Fixed-role account callbacks alone
may consume the floor, under existing exclusion; neither root UID nor capability
flags grant that bypass. Account headroom preflight checks both resources.
Extent rebuilding can exhaust its temporary credits slightly above the floor;
the floor is a minimum preserved capacity, not a promise every remaining block
can be consumed. Reclamation remains allocation-free.

Package preflight must budget its own staging, rollback and concurrent-log needs
in addition to the account floor. These free counts do not reserve package space.
There is no package allocation owner yet: generic reservations and package
mutation admission remain a package-manager design gate. The floor does not
reserve heap, dentry, journal credits or orphan slots, or protect against controller failure.
Live /mnt exhaustion policy is unchanged.

## Acceptance

Final-source campaigns:

- `make test-ext4-health-host`: **6/6 PASS, 36 independent Linux fsck audits**.
  Actual mounted EXT4/VFS/JBD2 under ASan/UBSan;
  1/2/4 KiB blocks × 512/4096-byte sectors. Concurrent four-writer append records,
  short-write prefix/offsets, pinned orphan health, lost close-error acknowledgement,
  transaction fencing, retired dentry with retained recovery intent, sticky flush
  failure, repeat sync/reboot, installed-policy block/inode floors, fixed-role
  account consumption and exact reclamation. A final additional gate fills the
  account publication directory while below the block floor and requires account
  rename to grow it from reserved space; its empty capacity remains owned.
  Trusted-root policy is selected by
  a host adapter on /mnt; this is not installed-root firmware exhaustion evidence.
  Final inode-floor runs reclaim 223 ordinary file inodes in each geometry;
  directory capacity can remain allocated after names are removed. Ownership is
  independently checked by Linux; no directory-shrink feature is claimed.
- `make test-ext4-health`: **4/4 PASS**. Release-excluded Ring 3 probe in isolated BIOS/UEFI
  SMP=1/4 QEMU fixtures. Existing SYS_MOUNTINFO/SYS_SYNC, unchanged oversized-write
  rejection, bounded write loops, replacement pins/cleanup flags, actual zero-free
  inode and block exhaustion, reclamation and clean shutdown. Independent Linux
  fsck and exact 64 KiB/three-byte payload audits. Disposable NVMe /mnt only.
- `make test-ext4-replace-fault-host`: **PASS**, final-source ASan/UBSan and eight
  Linux audits. Covers 1024 pinned legacy cache entries, zero free data blocks,
  pre-I/O allocation/read/journal-credit errors, 32 replacement/reader races,
  credentials and timestamp bounds.
- `make test-ext4-replace-host`: **12/12 PASS, 12,784 modeled interruption cuts**,
  normal/wrapped journals across 1/2/4 KiB blocks and 512/4096-byte sectors;
  before/after every activation/cleanup write/flush under four persistence modes,
  two recoveries, complete ownership/reference admission and 36 Linux
  fsck/namespace/payload audits. Existing modeled-sector boundaries apply;
  no physical or sector-tear guarantee. Final manifest: `build/ext4-replace/manifest.json`.
- `make test-fstat-host`: **PASS**, retained-object/error-precedence regression.
- `make test-ext4-time-host`: **6/6 PASS**, mounted timestamps plus Linux
  metadata/payload/fsck oracles; no EH-1B policy expansion.
- `python3 scripts/test_disk_host.py`: **20/20 PASS**, existing disk/df/bench and
  info CLI compatibility under ASan/UBSan. Specific health bit values are tested
  in the mounted host suite and Ring 3 probe.
- `make test-accounts-persistent-host`: **872 functional cases PASS**, including
  860 account flush-cut/recovery cases (215 boundaries × before/after ×
  512/4096-byte sectors), two write-failure cases, two installed-root host
  profiles and eight admission cases. Restored account sets receive independent
  Linux audits. **The command exits FAIL at its final artifact-preservation
  guard**: shared `bin/fortress.iso` and `bin/fortress.img` changed during the run.
  Its original FAIL is retained at `build/root-phase3/gate-jd_z0zib/manifest.json`;
  this is not an unqualified whole-target PASS. Manual reconciliation identifies
  three unrelated changed source paths: `user/sshd/sshd.c`,
  `scripts/create_installer_payload.py`, and `Makefile` (SSH service packaging).
  All recorded account/storage inputs, ABI and syscall implementation hashes
  are unchanged. The supplemental proof is
  `build/ext4-health/account-regression-reconciliation.json`.

Host manifests and logs: `build/ext4-health/` and `build/ext4-replace-fault/`.
Their recorded source hashes match the final working-tree runtime inputs.
Final firmware manifest:
`.codex-remote-attachments/ext4-health-20261020-172255/manifest.json`.
It records `bfebd29` as the unrelated build/TLS checkpoint and confirms no release
output changed during the campaign. Only the copied workspace's test ISO was
created. Each QEMU argv admits one disposable regular-file NVMe data disk, plus
its test CD and paired pflash for UEFI; the source fixture is unchanged.

No runtime source changes are permitted during a final campaign. Earlier runs
were superseded after adding the close-failure mutation fence and the scoped
account-rename directory-growth bypass. Debugging corrected
the fixture's assumption that extent rebuilds reach the exact block floor, an
account test payload above ACCOUNT_FILE_MAX, and a Ring 3 request above the
existing 16 KiB syscall cap. Early SMP=4 BIOS runs stopped during early console
output because the new harness used an invalid UTF-8 error-handler name when a
serial read split a multibyte character. That runner defect was corrected; those
logs remain separate from acceptance, without a kernel-failure attribution.

EH-1B timestamp policy remains separately open. TLS/HTTPS sources are untouched;
firmware builds freeze unrelated TLS/build inputs at the recorded committed HEAD
inside a copied workspace. This task built only disposable test artifacts and
performed no physical write, release build, commit or push.

The shared checkout advanced concurrently from `bfebd29` to SSH commit `9a56be3`.
That concurrent commit also captured EH-3's shared AGENTS.md/Makefile additions;
the ext4 runtime, new tests and evidence documents remain working-tree changes.
It was not created or rewritten by this task. Shared release artifacts were
rebuilt during that concurrent work; the earlier isolated EH-3 firmware campaign's
own unchanged-output guard still passes for its recorded interval. Do not infer
whole-session artifact preservation from that narrower result.
