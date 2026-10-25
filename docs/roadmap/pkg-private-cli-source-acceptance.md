# Private package CLI and authenticated source acceptance

This batch continues `9098755` while preserving `55b97a6` (ABI work).
Production mutation enablement, trust provisioning, canonical release builds,
pushes and physical-device writes remain disabled.

## Implemented private path

The private boot profile authenticates its single catalog entry and derives
the package identity from it before preparing the source. It verifies the
artifact against that identity through the existing executor. It does not
guess a package from an unauthenticated manifest. Mutation requests accept a
canonical 1–32 byte identity and must match the immutable prepared source.
Malformed or unknown identities do not select another source or spend escrow.
The fixture producer supports different package and command identities; the
combined boot campaign uses `field-kit` / `field-tool` rather than the old
`toolbox` / `hello` names.

`pkg_private_cli` supplies real private syscall callbacks to the existing CLI.
Status is read-only. Install/remove/recover lazily acquire a fresh owner session
and release it after the command, including failed results and output. No
mutation/recovery is retried after a lost result. The copied probe exposes this
entry as `/bin/pkg-boot --pkg VERB [PACKAGE]`; tests spawn separate Ring 3
commands and wait for their exit status. Default `/bin/pkg` remains read-only.

Only one authenticated immutable source is admitted per private boot. Local
file/download ingestion, a multi-package registry and upgrades are outside
this batch, as agreed with the user. The source module retains its 32 KiB cap.
The existing 16-file, 64-directory and memory/journal limits are unchanged.

## Baseline and retained failures

The pre-freeze run `pkg-exec-boot-20261025-150228-661780` completed after resume:
8/8 normal BIOS/UEFI × SMP=1/4 recovery/reboot boots, four intentional durable
COMMIT cuts and two no-write refusals (wrong key and foreign account stage).
Its source snapshot matches the frozen source tree. Its original final receipt
reports runtime PASS and preservation PASS, but the old direct runner checked
only source changes at completion, missing later canonical guard changes.

The separate retained `independent-preservation.json` reports **preservation
FAIL**, without rewriting that receipt: Makefile, AGENTS.md and canonical
kernel/image/ISO/manifest/bootstrap hashes changed before the freeze. No source
hash changed. Original final receipt SHA-256:
`4041a061648410eb8718871c1647a27f9a0f53f83dc32cc2c83c93b9f0a5472a`.
The final runner now rechecks sources, canonical guards and copied inputs.

`pkg-exec-boot-20261025-150113-883903` retains a snapshot/preservation failure:
the ABI header changed during copying; no compilation or runtime acceptance.
`pkg-exec-boot-20261025-152913-708704` retains a build failure with preservation
PASS: the new child-command test compared unsigned exit status with a signed
expectation. The private kernel compiled; the test's expectation type required
correction. A preliminary draft host test also used unavailable `strcpy` through
the project string header; replacing that test call with `memcpy` fixed it.
These failures are not runtime passes.

`pkg-exec-accept-20261025-153535-451455` retains runtime FAIL, preservation PASS.
Four BIOS normal boots and three COMMIT cuts had completed. UEFI/SMP=1 then
returned zero with QMP `guest-shutdown`, but its socket-backed serial log stopped
during initramfs enumeration and lacked all required package markers. It is
not counted as accepted. Separate diagnostic extraction passes Linux fsck and
all sixteen member hashes and finds sequence 1776/generation 10/IDLE; the
ephemeral MAC key was already retired, so this diagnosis does not verify HMAC
or substitute for runtime acceptance.

The existing UART transmitter permanently disables serial output after its
bounded wait fails. Socket backpressure is a plausible explanation for the
lost output, not an independently proven root cause. The autonomous private
runner now uses file-backed UART capture, needs no UART input, and rereads the
final log after process termination. Kernel UART behavior remains unchanged.
The failed receipt and its serial/QMP/process/debug/disk evidence are retained.

## Combined verification

Commands run in WSL Ubuntu-24.04 at `/mnt/c/Sources/FortressOS`:

```sh
python3 scripts/test_pkg_exec_cli.py
python3 scripts/test_pkg_install_fs.py
python3 scripts/test_pkg_exec_boot.py --build-only
python3 scripts/test_pkg_exec_accept.py .codex-remote-attachments/pkg-exec-boot-20261025-155745-671901
python3 -m py_compile scripts/pkg_exec_overlay.py scripts/test_pkg_exec_boot.py scripts/test_pkg_exec_accept.py scripts/test_pkg_exec_cli.py scripts/pkg_install_fixture.py scripts/build_pkg_exec_frozen.py
make -n test-pkg-exec-cli-host test-pkg-exec-boot
```

Private CLI receipt: `pkg-exec-cli-20261025-152743-000935`, runtime PASS and
preservation PASS. Actual parser/frontend code runs under ASan/UBSan with mocked
syscalls. It covers lazy acquisition, read-only status, recovery, release on
output/result failures, acquisition/release failures, malformed snapshots,
32-byte names, argument rejection and no mutation/recovery retries. Maximum
freestanding frontend frame: 256 bytes (1024-byte ceiling).

Executor regression receipt: `pkg-install-20261025-152827-947426`, runtime PASS
and preservation PASS. All four profiles pass: one file, sixteen files, a 4 MiB
executable with fifteen documents, and insufficient capacity. Successful
profiles each cover 26 interruption recoveries plus repeat recovery, authority
loss at four durable phases, and independent content/ledger/Linux fsck checks.
The low-space case refuses before reservation. Foreign descendants are refused
in the sixteen-file and large profiles. Maximum measured private host kernel
surface frame: 1008 bytes. The subsequent correction changes only the separate
Ring 3 child-command test's expected-status type; executor inputs are unchanged.

Initial fresh private build: `pkg-exec-boot-20261025-153352-497165`, build PASS,
preservation PASS, runtime NOT RUN. It builds only the copied kernel and private
probe/CLI/bootstrap/fixture ISO. Maximum measured executor/binding frame:
960 bytes (1024-byte ceiling).

- Kernel SHA-256: `03c7f66d67b594df93be54cbac6de05b8177d023c315c43283ae23a22d9155a3`.
- Bootstrap SHA-256: `bcd11593dbf7b4a558f0ec2f30700e31db72b9d5c2ac51d5df6316add8aaa49d`.

Final file-capture build: `pkg-exec-boot-20261025-155745-671901`, build PASS,
preservation PASS, runtime NOT RUN; executor/binding maximum remains 960 bytes.

- Kernel SHA-256: `3f798e58cd245ffa7012ec5ed6b237f11cdee4b5408e8ece7da5f4421ad9a3ce`.
- Bootstrap SHA-256: `90bcdc7a11a86aa94ba219d554fea92b7e3a40eb9a0a49277acceb041ac52926`.

The acceptance
runner now rejects a stale source snapshot or Makefile and verifies the frozen
kernel/bootstrap hashes before launching QEMU. A build-only option separates
private compilation from repeated fresh-fixture acceptance. The refresh tool
recompiles the CLI objects instead of assuming the old object count.

Final acceptance receipt: `pkg-exec-accept-20261025-155923-784236`, **runtime
PASS, preservation PASS**. Its eighteen case receipts comprise:

| Cases | Result |
| --- | --- |
| BIOS/UEFI × SMP=1/4, recovery and clean reboot | 8/8 normal boots PASS, exit zero and QMP guest-shutdown. |
| Durable-COMMIT emulator termination | 4/4 intentional CUT receipts; recovered by the following boot. |
| Wrong key, foreign account stage, foreign executable, stale transaction | 4/4 refusal PASS, zero writes/flushes and unchanged disk bytes. |
| Damaged catalog and signed catalog/artifact identity mismatch | 2/2 refusal PASS before writes at signed-source admission. |

Every normal boot completes the real command-entry tests, Linux e2fsck, sixteen
member hash/uid/gid/link checks, seven measured base hashes and independent
ledger HMAC/floor/closed-ownership/reservation checks. Completion has sequence
1776/generation 10; reboot completion has sequence 3428/generation 18. Floors
remain 73/3/7. Bytes outside the selected partition remain unchanged.

- Acceptance manifest SHA-256: `e9b6035d804a8e5561cf360510a0cee6eeb87e8654affc4a3b3a8e19e7d23db6`.
- Final build manifest SHA-256: `f385a453eabfabe012767741b1748c27e314fb56d3ffe9d5bf5b976d5099d5df`.

Python compilation and both Makefile dry runs pass; the latter list only private
runners and the existing-bootstrap assertion, with no release build dependency.
The original production integration plan remains byte-identical. A separate
final inventory/hash check is retained as `independent-final-preservation.json`.

Runtime acceptance is separate from source/media preservation. Neither host
fault injection nor QEMU COMMIT cuts prove physical power-loss safety. This
campaign does not establish interactive login/SSH mutation, native account
writers, execution of the installed fixture application, external trust-floor
advancement or production key custody. Existing limitations and failures in
[the earlier audit](pkg-kernel-mutation-acceptance.md) remain recorded.
