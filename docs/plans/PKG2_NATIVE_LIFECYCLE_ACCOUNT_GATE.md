# Native recovery lifecycle and detached account gate

2026-10-24. Continuation of `91d5487`. All implementation is in private test
modules and overlays applied to copied workspaces. Production dispatch, boot
selection and account services are unchanged. Installation, provisioning,
release-media builds, pushes and physical writes remain disabled.

## Implementation and source trace

`pkg_native_integration_overlay.py` layers on the measured, selector-authenticated
pending-state backend. After projected authentication/replay and reopened-root
validation, it constructs a detached VFS root with the existing `e4_setup`
helper, then invokes `ext4_root_account_namespace_admit`. Package admission is
published only after this construction succeeds. No global VFS root is replaced.

`pkg_native_integration.inc` binds readiness to that exact retained mount.
Pending, invalid or tainted state denies account preparation and the copied
`ext4_publish_root` entry point. A nonblocking atomic preparation gate serializes
the account handoff; readiness is published only after actual account recovery,
database/home validation, its durability barrier, and renewed package validation.
The ready state is boot-lifetime and repeated preparation does not repeat I/O.

An isolated instance of the actual `src/kernel/accounts.c` is compiled into the
private kernel. Its storage uses the real detached-root fixed-role account
callbacks and `account_redo_recover`, with the real coherent database/home
validator. There is no synthesized account-state conversion or authentication
stub. Its symbols/static state are separate from the live login service.
Private syscall 202 delegates request copying/range and shadow-permission checks
to that service. BEGIN/UPDATE/FINISH remain read-only refusals: the accepted
account-generation-v3 binding is immutable in this profile.

The account barrier permits only the actual fixed-role sync callback; package
mutation authorization is not left globally open around account preparation.
No new lock rank is added. Package/process/owner checks occur outside filesystem
exclusion. `jn_boundary` uses the existing interruptible `sched_wait_until`
outside all locks. The private credential-loss control updates the real process
credential record, releases its lock and wakes the exact wait channel.

The existing `pb_control` retains the owner operation until its callback unwinds.
`pw_complete` precedes the normal syscall-return signal/exit path; private
`gk_owner_exit` then cancels the writer, and reaping checks the owner key is gone.
Spawn creates a new TCB/key instead of inheriting the session. The current syscall
ABI has no in-place exec operation; no new in-place-exec acceptance is claimed.

Native fault injection returns failure after a successful NVMe sector write or
flush, modeling acknowledgement loss. The filesystem latches taint; status,
recover, acquire, account access and publication then refuse without additional
storage operations. A native test exposed the old private validator's combined
taint/orphan `EINVAL` return. Taint now returns `EIO` separately; orphan/format
refusals retain their existing errors. Fresh boot reauthenticates and recovers.

## Combined verification

Final campaign: `.codex-remote-attachments/pkg-boot-admission-20261024-191210-956586`.
`integration-manifest.json` reports **32/32 boot runtime PASS and preservation
PASS**, with no changed repository sources, shared outputs or copied inputs.
Boot runtime and preservation are separate claims.

Across BIOS/UEFI × SMP=1/4, each configuration runs eight boots: full lifecycle
and account preparation; its actual result reboot; sector acknowledgement loss
and reboot; incoherent home and reboot; account-sync acknowledgement loss and
reboot. The normal initial boot checks eight actual owner children/reaps and
three signal cases, plus a subsequent unprivileged account-snapshot child.
No new storage operations occur on tainted-state retries. Private account
preparation succeeds again on each valid fresh boot.

Every positive recovery image matches the independent actual-EXT4 reference
byte for byte. Its only additional account operation is one sync barrier, with
no account-file writes. Sector acknowledgement loss permits changes only inside
the journal; all other disk bytes, GPT and unrelated sentinel remain identical
to the input. Linux `e2fsck -fn` succeeds on all 32 outputs.

`grant-audit.json` independently reads the sealed grant from every actual output:
the four failed-sector cases retain home PENDING/sequence 2 while the kernel
denies operations with EIO; the other 28 have IDLE/sequence 7. The inherited
runner's `sequence=1` JSON field is a compatibility argument, not an observed
transaction sequence. Original results are retained alongside this explicit
grant audit. Post-sync-failure home state is durably IDLE, but the kernel still
correctly refuses authentication/publication for that tainted boot.

New private filesystem frames are <=272 bytes; the combined audited private
filesystem maximum is 448 bytes, below 1024. The actual isolated account service
also passes the existing linked private frame/symbol audit. Only the previously
approved verifier exception retains its 1280-byte ceiling. Compiler commands,
source snapshots, overlays, measurements, complete QEMU argv and UART logs are
retained in the campaign directory.

Exact final top-level command, run from the repository in WSL Ubuntu-24.04:

```sh
python3 scripts/test_pkg_native_integration.py --native /mnt/c/Sources/FortressOS/.codex-remote-attachments/pkg-pending-20261024-182509-217854/pending-native
git diff --check
```

The reference executable reads the saved signed input fixture into its fault
model and writes a new expected output file. It does not mutate its input.
Final measurements and every package/canonical-account byte are verified
against the current private kernel/bootstrap/capsule, not shared release files.

Retained earlier campaigns beneath `.codex-remote-attachments/`:

- `pkg-boot-admission-20261024-185412-947143`: runtime FAIL, preservation PASS.
  The probe omitted explicit R10=0 for SETRESUID's fourth flags argument. The
  corrected helper now supplies all four ABI arguments; no kernel workaround.
- `pkg-boot-admission-20261024-185746-439900`: positive lifecycle/reboot checks
  passed, then lost-write-ack taint status returned EINVAL and failed the EIO
  assertion. Runtime FAIL; preservation FAIL records concurrent development of
  the four new integration source/harness files; copied inputs unchanged.
- `pkg-boot-admission-20261024-190138-602441`: private build completed but runtime
  NOT RUN; preservation FAIL recorded the evolving probe and taint fix. No boot
  acceptance is inferred from this build.
- `pkg-boot-admission-20261024-190253-313105`: earlier 24-boot runtime PASS and
  preservation PASS, before additional stale/inherited-session checks and the
  account-sync loss case.
- `pkg-boot-admission-20261024-190930-831341`: earlier 24-boot runtime PASS,
  preservation FAIL from adding the sync-loss coverage to the four integration
  files while the campaign ran. Copied inputs unchanged. This original FAIL is
  retained; the final 32-boot campaign supersedes it without relabeling it.

The native probe checks effective-UID/capability loss, competing owners, inherited
and stale sessions, invalid request/output ranges, normal exit, SIGKILL after one
durable step, SIGTERM with STOP/CONT, caught SIGINT, credential loss while paused,
real child reaping, fresh-owner reuse and post-commit copyout failure. Repeat
recovery must add no steps/writes. Signals are injected between durable steps;
this does not claim signal injection during an in-flight controller command.

Account tests exercise real STATUS/SNAPSHOT/CHECK, invalid output pointers,
unprivileged shadow rejection, denied account mutation, repeat preparation and
publication denial before account validation. A `/root` mode mismatch is
accepted by the package-only proof but rejected by the actual account validator;
neither account access nor publication becomes ready. A lost account-sync
acknowledgement likewise leaves private account access/publication closed until
reboot. Both failures are checked before any publication readiness is granted.

## Remaining scope

The selected root stays detached. The positive publication check is a readiness
preflight; rejected publication calls enter the actual guarded function, while a
successful preflight deliberately does not replace the running live root. A full
installed-system root handoff, live service authentication switch, clean shutdown
and incompatible-kernel/rollback policy remain separate integration work.

The account service instance is private and read-only, with the root-only signed
fixture database. Mutable account generations, concurrent account writers, public
ABI assignment and generalized package install/remove are not enabled. Native
lifecycle acceptance here starts from the bounded staged PENDING profile; other
package-state/layout bounds retain their separately scoped earlier evidence.

Read-only Linux checks can warn that journal recovery is skipped; they establish
structural/oracle acceptance, not a clean-bit or physical durability claim.
No physical-device acceptance is inferred from host models or QEMU.
