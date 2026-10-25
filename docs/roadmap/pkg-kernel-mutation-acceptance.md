# Private kernel package mutation acceptance

This checkpoint binds the bounded executor from `f2ca7ed` to retained kernel
owners, measured installed-root admission and the real account service in a
copied test kernel. Default production mutation remains disabled. See the
[binding contract](../plans/PKG2_KERNEL_MUTATION_BINDING.md) for the implemented
transport, source limits, admission order and owner lifetime trace.

## Reproduction and retained evidence

Run in WSL Ubuntu-24.04 at `/mnt/c/Sources/FortressOS`:

```sh
python3 scripts/test_pkg_exec_boot.py
# A retained, hash-verified private build can be refreshed and tested separately:
python3 scripts/build_pkg_exec_frozen.py .codex-remote-attachments/pkg-exec-frozen-20261025-141359-062444
python3 scripts/test_pkg_exec_accept.py .codex-remote-attachments/pkg-exec-frozen-20261025-142609-548551
python3 -m py_compile scripts/pkg_exec_overlay.py scripts/test_pkg_exec_boot.py scripts/test_pkg_exec_accept.py scripts/build_pkg_exec_frozen.py
```

The frozen refresh builds only `bin/fortress.elf` inside a new private copy,
compiles the Ring 3 probe and relinks the actual mutation CLI objects. Its input
workspace is hash-checked and retained unchanged. The acceptance runner checks
the current implementation against those compiled inputs before launching.
Private ISOs and fresh regular-file NVMe fixtures are permitted; these commands
do not build canonical release media. The local MAC key is temporary, external
to the media, and checked absent from the ISO.

Final frozen build: `pkg-exec-frozen-20261025-142609-548551`.

- Kernel SHA-256: `ce3febf903ee296bc0584a181b519eea705baeef4effcfe3d5a5ad4151a6036d`.
- Bootstrap SHA-256: `84b795c0475084b2a5bcacacdd03eead5a350d29cd2182a84555b908107d8dc9`.
- Acceptance receipt: `pkg-exec-accept-20261025-142956-145914` under
  `.codex-remote-attachments/`.
- Final receipt SHA-256: `51f488a7e9b957fe6ff0b53405cba4841d931846640e32d8ee3b69db80fd3edf`.
- Build receipt SHA-256: `4b2b1b105d9abfac32ba0769ab893a3972a01536b4ae68f1ae50bb58dac111f3`.
- Maximum measured private executor/binding frame: 960 bytes (ceiling 1024).

Final result: **runtime PASS, preservation FAIL**, with no runtime error and
`changed_copies=[]`. Sixteen case receipts comprise:

| Cases | Result |
| --- | --- |
| BIOS/UEFI × SMP=1/4, recovery after cut and subsequent reboot | 8/8 normal boots PASS; every process exits zero with QMP guest-shutdown. |
| Durable-COMMIT emulator termination for each configuration | 4/4 intentional CUT receipts; each resulting disk is recovered by the next accepted boot. |
| Wrong local key, canonical foreign account stage, foreign executable, stale transaction entry | 4/4 refusal PASS; zero writes/flushes and whole-disk bytes unchanged. |

All eight normal outputs pass Linux e2fsck, sixteen member hash and uid/gid/link
checks, seven measured base-file hashes, independent ledger HMAC verification,
closed/full ownership masks, zero residual reservations and floors 73/3/7.
Initial completion has ledger sequence 1363/generation 8; reboot completion has
sequence 2602/generation 14. This validates durable state across the reboot;
it does not prove an independently advanced external rollback floor.

Python compilation and `make -n test-pkg-exec-boot` also pass. The dry run lists
only the existing-bootstrap assertion and private runner, with no release-media
prerequisite. `git diff --cached --check` passes for the package-only staged diff.

Each case retains QEMU arguments, serial/error/debug/QMP logs, process return
codes and its result. Completed boots additionally retain Linux `e2fsck -fn`
and independent content/base-file/ledger checks in `oracle.json`. Intentional
COMMIT cuts are neither clean boots nor successful shutdowns. Normal acceptance
requires exit code zero and a QMP `guest-shutdown` event, not just a boot marker.

## Retained failures and qualifications

Earlier receipts are retained, including these failures; later passes do not
rewrite their results:

| Receipt suffix (all dated 20261025) | Result / disposition |
| --- | --- |
| `092839-000654`, `093156-114595` (`pkg-exec-boot`) | Compiler failures: port helper/formatting and missing delegated-commit prototype; corrected. |
| `093333-334911` | Catalog refused before writes because fixture RTC preceded issuance; corrected fixture clock. |
| `093630-365570` | Probe misclassified startup `--installed`; corrected argument handling. |
| `093918-828449` | Earlier direct-wire smoke passed; preservation failed during edits. Superseded by actual CLI coverage. |
| `094515-306644`, `095016-659208`, `122927-252382` | Early BIOS startup exits in parallel development runs; cause not established. Isolated repeated BIOS boot passed, but no general parallel-startup fix is claimed. |
| `095340-989500` | Snapshot rejected a source change during copying. |
| `095444-480011` | Parallel lifecycle timeout; retained as failure. |
| `124200-807035` (`pkg-exec-accept`) | ISO command lacked `--efi-boot-part`; corrected producer. |
| `124342-481926`, `124722-942145`, `133057-901353` | Earlier isolated runtime passes with preservation FAIL during source/harness or concurrent work. The last included eight normal boots, four cuts and four refusals. |
| `135350-329705` | Runtime FAIL, preservation PASS: new test passed null to the existing CLOEXEC sweep, violating its valid-TCB precondition. Fault resolved to `fd_close_cloexec` (CR2 `0x4b8`). Corrected test invokes only the new null-safe owner hook. |
| `140958-735267`, `141858-085012` | Superseded runs intentionally interrupted with SIGINT to release CPU for the final probe. Their manifests retain FAIL/KeyboardInterrupt and preservation FAIL, not completed acceptance. Partial case results remain on disk. |

Source/media preservation is a separate result from boot acceptance. Concurrent
repository work is preserved, not reverted to improve a guard. The original
`docs/plans/PKG2_PRODUCTION_INTEGRATION.md` retains SHA-256
`f1e8a769becaac55509a82a1633fda9c6057167588becab8c9cba91521fb1017`.

The final campaign ran against its unchanged frozen copy while shared kernel
sources and canonical outputs changed. The source changes include signal ABI,
main, process-table, thread and IDT files; guard changes include Makefile,
AGENTS.md and canonical kernel/bootstrap/media/manifests. Concurrent crash-dump
work was committed as `b7af558`. This package runner never builds or writes those
canonical outputs. Their preservation FAIL is retained, and these boot results
do not accept the subsequently changed shared kernel. Overlay transformations
were separately checked against a temporary copy of the current tree; that
checks applicability only, not compilation or boot acceptance of concurrent work.

## Delivery boundary

The fixture automatically starts a privileged Ring 3 probe, not an interactive
login or SSH campaign. It installs one registered package with a small
structurally valid executable and fifteen documents using the exact
64-directory bound. It checks an open executable's retained bytes; it does not
execute that fixture application. The separate native executor campaign owns
the 4 MiB size, low-space and broader interruption evidence.

Immutable account lineage is admitted before replay and prepared by the actual
account service; account mutation is EROFS in this profile. Concurrent native
account/log writers and full interactive CLI campaigns remain broader Gate 4
work. Boot requires a currently valid catalog, including pending recovery;
expired-source recovery is not accepted. The terminal taint/namespace fence is
implemented but has no new hardware-error-injection campaign here.

Durable-COMMIT emulator termination is not exhaustive sector-cut or physical
power-loss acceptance. Production key custody, external floor advancement,
whole-disk rollback policy and firmware enforcement remain operational review
boundaries. Installation enablement, production trust provisioning, canonical
release-media builds, pushes and physical-device writes remain disabled.
