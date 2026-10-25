# Normal-build package integration and interactive candidate

2026-10-25. Batch based on `fba5313`, preserving `9098755`, `55b97a6` and
`0b23223`. Implementation uses the isolated `codex/pkg-system-integration`
worktree; shared-tree development is unfrozen. This is a Fortress 1.0
integration checkpoint, not production package enrollment or release acceptance.

## Implementation

The normal initramfs recipe now includes `/bin/pkg`. Its fresh command sessions
use the versioned [SYS_PKGCTL 95 contract](../plans/PKG_SYSTEM_ABI.md). Owner,
backend and service modules compile normally; exit/exec/reap hooks preserve the
accepted generation and cancellation protocol. The planner and filesystem
executor have moved into normal source modules. Existing candidate filenames
are compatibility includes so host and private-kernel tests exercise those
actual bodies rather than a second implementation.

The executor defaults to compilation off. Ordinary boots publish no package
backend and return EOPNOTSUPP before owner allocation or storage callbacks.
Private copied kernels enable the normal executor and publish the service only
after authenticated projected admission, recovery, account preparation and root
publication. QEMU identities, external TEST MAC delivery, signing fixtures,
fault controls and private syscalls remain test-only. The expanded measured
login/SSH fixture uses a new B2 selector domain, independent ephemeral SSH host
identity and canonical account directory slots.

## Retained host and default-build evidence

Receipts below are relative to `.codex-remote-attachments/` in the retained
worktree, unless explicitly stated otherwise. Runtime and preservation are
separate manifest fields.

| Command / scope | Receipt | Result |
| --- | --- | --- |
| `python3 scripts/test_pkg_system_host.py`: actual service/owner/backend, input/output validation, disabled admission, credentials, owner isolation, busy credit, mutation failure, no retry, exec/exit/reuse | `pkg-system-host-20261025-185431-822663` | runtime PASS; preservation PASS |
| `python3 scripts/test_pkg_exec_cli.py`: actual normal frontend, mocked v1 transport, fresh sessions and output failures | `pkg-exec-cli-20261025-185442-169288` | runtime PASS; preservation PASS |
| `python3 scripts/test_pkg_writer_lifetime.py --backend`: actual owner/backend and process registry | `pkg-backend-20261025-180823-493503` | runtime PASS; preservation PASS |
| `python3 scripts/test_pkg_plan.py`: extracted planner vectors | `pkg-plan-20261025-184050-806510` | runtime PASS; preservation PASS |
| `python3 scripts/test_pkg_install.py`: extracted filesystem executor, four profiles, successful-profile interruption cuts and independent Linux oracles | `pkg-install-20261025-184050-818119` | runtime PASS; preservation PASS |
| Default executor-off preprocessor comparison, original versus integrated filesystem, same include resolution | `executor-extraction/default-preprocessor-result.json` | whitespace-normalized output identical |
| `make test-abi-version-host` | normal worktree build output | PASS; syscall audit 96/96 |
| `make -j8 bin/fortress.elf build/pkg/pkg.elf` and `make -j8 bin/initramfs.tar` | normal worktree components | PASS; `/bin/pkg` mode 0755; no canonical ISO/IMG rebuilt |
| Normal unenrolled BIOS/SMP=1 and UEFI/SMP=4, normal CLI help/status/install/recover, clean shutdown, no writable disk | `pkg-default-20261025-190112` | 2/2 runtime PASS; component preservation PASS |
| `python3 scripts/test_ssh_hostkeys_host.py` | `build/ssh-hostkeys-host/result.json` | sanitizer/codec/live identity gates and 266 independent interoperability cases PASS |

The normal kernel symbol audit finds package service/lifecycle symbols, with no
private mx_/gk_ adapters or enabled px_ executor. Default boot tests do not prove
admitted mutation; host tests do not prove interactive or physical acceptance.
The final bootstrap, including the SSH descriptor fix, repeats the default
two-boot check as `pkg-default-20261025-193030`: runtime PASS, preservation PASS.
Normal kernel SHA-256 is
`693742f4073b8dd60303581f2bce5745603233650a4f4dc87e145121cc820126`;
normal bootstrap SHA-256 is
`599a832081f02e0a1ff0be784d6ea9f19f393e4c61fa4920e986317ddcb41972`.

Interactive testing exposed inherited SSH worker/PTY descriptors. The PTY pair
now uses CLOEXEC; spawn DUP2 preserves slave stdio while closing the original
master/slave descriptors on shell exec. The worker also marks its transferred
client/monitor descriptors CLOEXEC before starting a shell. Kernel PTY signal,
session, lock and descriptor semantics are unchanged. ASan/UBSan suites
`python3 scripts/test_ssh_pty_host.py` and
`python3 scripts/test_ssh_daemon_host.py` PASS; actual disconnect cancellation
is a separate runtime gate below. Private lifecycle counters now delegate to
the normal package exit function, and exec uses the normal hook directly.

## Interactive and interruption acceptance

The interactive campaign `pkg-system-interactive-20261025-193439-782169` and full-matrix interruption/refusal campaign `pkg-exec-accept-20261025-193640-201701` complete candidate verification on disposable enrolled roots across BIOS and UEFI with SMP=1 and SMP=4.

| Campaign / scope | Receipt | Result |
| --- | --- | --- |
| `python3 scripts/test_pkg_system_interactive.py`: live login/sudo, SSH session, unprivileged rejection, runnable package execution, paused SSH disconnect with PENDING/RETIRING cancellation and reboot recovery to IDLE across 8 boots (BIOS/UEFI × SMP=1/4 initial + reboot) | `pkg-system-interactive-20261025-193439-782169` | 8/8 runtime PASS; preservation PASS |
| `python3 scripts/test_pkg_exec_accept.py`: full matrix across BIOS/UEFI × SMP=1/4, 4 intentional COMMIT cuts with verified crash recovery, 8 standard runs, and 12 security refusals (sequence rollback, corrupted root, invalid signature, bad catalog) | `pkg-exec-accept-20261025-193640-201701` | 24/24 runtime PASS; preservation PASS |

### Interactive Session Details (`pkg-system-interactive-20261025-193439-782169`)

The interactive suite validates actual Ring 3 `/bin/login`, `/bin/sudo`, and `/sbin/sshd` sessions driving `/bin/pkg` over SYS_PKGCTL:
1. **Unprivileged rejection**: Operator running `/bin/pkg install field-kit` without root privileges fails with exit 1.
2. **Sudo installation**: `sudo /bin/pkg install field-kit` completes with exit 0, populating `/usr/bin/field-tool` and shared documentation.
3. **Runnable package execution**: Direct execution of `/usr/bin/field-tool` returns exit 0 with expected behavior.
4. **Clean removal and recovery**: `/bin/pkg remove field-kit` (with checked credentials) and `/bin/pkg recover` execute cleanly to status IDLE.
5. **Interactive SSH session**: Starting `/sbin/sshd -D &` over PTY duplex bridge, connecting authenticated client, running `/bin/pkg status`, unprivileged install rejection, sudo install, `/usr/bin/field-tool` execution, sudo remove, and sudo recover.
6. **Concurrent pause and disconnect cancellation**: Under `/bin/pkg-boot --pause`, a package mutation is initiated and paused. Disconnecting the SSH client delivers SIGHUP and cancels the in-flight command from PENDING/RETIRING state; `/bin/pkg-boot --release` followed by `/bin/pkg recover` cleanly drains and restores status IDLE.
7. **Offline Linux Oracle**: 16 members, 11 base hashes preserved, sequence 2602, generation 14 verified clean with independent e2fsck/debugfs.

## Retained failures

- `pkg-exec-boot-20261025-183908-447066`: strict-warning failure in a private
  adapter; misleading indentation corrected. No runtime acceptance claimed.
- `pkg-system-interactive-20261025-184317-444873` and
  `pkg-system-interactive-20261025-185257-312608`: projected admission refused,
  writes=0/flushes=0. Expanding base inventory had incorrectly enlarged the
  eight-anchor loop in the test adapter; fixed using the actual array bound.
- `pkg-system-interactive-20261025-185826-171226`: account-lineage refusal,
  writes=0/flushes=0. Host debugfs mkdir consumed a reserved account slot;
  fixture now explicitly retains SSH while restoring canonical account slots.
- `pkg-system-interactive-20261025-190130-584646`: console package operations
  passed; harness attempted an unsupported asynchronous compound shell list.
  Startup now uses separate shell commands.
- `pkg-system-interactive-20261025-190536-858656`: console package operations
  passed; SSH host-key readiness refused. Runtime FAIL retained. Preservation
  FAIL also retained: this controller was edited during that development run.
- `pkg-system-interactive-20261025-191428-525789`: guest host-key syscall
  returned EIO. The private adapter set mounted/trusted flags after root-node
  setup, omitting retained callbacks. Its order now matches the normal
  prepared-root constructor. Guest host-key load subsequently passes.
- `pkg-system-interactive-20261025-192149-679389`: console and SSH package
  operations passed, but disconnect left the paused command able to complete
  to IDLE. This is the retained failure motivating the SSH descriptor fix;
  the required PENDING/RETIRING cancellation outcome was not relaxed.
- `pkg-exec-boot-20261025-185513-865920`: overlapping development build discovery
  failed and sources changed during build; no runtime acceptance claimed.
- `pkg-exec-boot-20261025-190451-590640`: build-only, preservation FAIL because
  its host controller was corrected during the build. Not used as final proof.
- `pkg-exec-accept-20261025-190948-636890`: Windows managed-worktree Git link
  could not be resolved by Linux Git. Controller now translates the explicit
  Git directory for read-only metadata collection; no boot ran in this receipt.
- `pkg-exec-accept-20261025-191112-243502`: superseded campaign interrupted
  after a passing BIOS/SMP=1 COMMIT cut and recovered boot. Runtime FAIL and
  preservation FAIL remain recorded (controller/source edits during the run);
  no full-matrix claim. Cancellation receipt records the reason.
- `pkg-exec-boot-20261025-191718-602164`: test diagnostic overlay expected a
  unique sync call where three existed; restricted replacement to the read
  function. `pkg-exec-boot-20261025-191857-449628` was an overlapping build
  discovery/preservation failure. Neither is runtime evidence.
- `pkg-exec-boot-20261025-192954-103622`: removed private exec helper still had
  one test-control caller; changed that caller to the normal hook. Compile
  failure retained; succeeding build uses the normal hook.

No admission rule was relaxed to get past these failures. Reuse checks retain
exact compiled input/kernel/bootstrap hashes. A host controller may differ from
its build only by its own script; that difference is explicitly recorded, while
the running controller and all other source/guard/copy hashes stay frozen for
the campaign. This differs from silently accepting stale guest inputs.

## Read-only Dell inspector

`pkg-inspector-20261025-193040/manifest.json` records a separately copied
`INSTALLER_INSPECT=1` kernel and identity-only tool, built without canonical
media publication. Its ISO is `fortress-nvme-inspect.iso`, SHA-256
`05b777c99803167d1beab5c0031874466c2d8a0e203a203a23876ac0a2bfbee6`.
The retained producer is `.codex-remote-attachments/prepare_inspector.py`.

Existing `scripts/test_installer_inspect.py` assertions ran with only KVM/host
replaced by TCG/max in a retained controller copy. Receipt:
`build/root-phase4/inspect-kn1hv04s/manifest.json`. BIOS and UEFI both PASS;
writable disposable 384 MiB sentinel disks have identical before/after SHA-256
`c1590a00b735784d4469ddf9ca235b92abded31959d2aa7ab98f8cd9514d1a45`.
The UEFI screenshot was inspected: actual model/serial/capacity/sector size,
PCI/namespace and identity digest display; installation and sector operations
are unavailable. No physical device was accessed.

First Dell action is to boot this identity-only inspector, photograph its
identity screen, then power off and remove the inspection medium. Compare with
the recorded spare in the [Fortress 1.0 plan](../plans/FORTRESS_1_0_INTEGRATION.md).
This tool does not inspect GPT, filesystem health, free space or package
enrollment. Those require a subsequent read-only procedure before constructing
any bound write candidate. Existing installation preservation remains required.

## Limits

Production enrollment/key custody, external minimum-sequence management,
older-kernel exclusion, independent recovery and hardware-shaped admission
remain unimplemented. General/local/downloaded package ingestion, multiple
packages, upgrades and live account mutation are outside the accepted fixture.
An interactive disposable-root pass must not be described as ordinary installed
hardware package enablement. No production trust provisioning, release-media
build, push, physical-device write or physical mutation acceptance is performed
by this batch.
