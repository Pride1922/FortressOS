# Private enrollment geometry prerequisite

2026-10-24, continuing checkpoint `f2e162e`. This is a completed structural
prerequisite for Gate 2 native enrollment, not completion of Gate 2.

`scripts/pkg_enroll_fixture.py` creates a new 32 MiB EXT4 filesystem inside a
new 64 MiB GPT regular file. There is one selected Linux-filesystem partition
and one unrelated 2 MiB sentinel partition. Both GPT tables/headers are emitted
with CRC32 checksums. UUIDs are explicit public TEST identities. No existing
filesystem is enrolled or modified. The filesystem contains root-only account
files, empty account staging and transaction directories, and dense placeholder
package-record files. The placeholders are deliberately **not signed records**.

The native account namespace has a stricter capacity grammar than ordinary
Linux directory slack: `/etc` needs six 24-byte slots and the private account
directory needs five. The producer lays out these slots and independently
calculates their UUID/inode/generation-bound EXT4 directory CRC32C checksums.
Linux `debugfs` sets target ownership to root; host user identity is not used as
target authority. This fixture currently retains Linux-generated inode
generations, including zero. Enrollment generation certification remains open.

`tests/pkg_enroll_geometry_host.c` compiles actual GPT, EXT4/JBD2 and account
namespace validation code from copied sources under ASan/UBSan. GPT primary and
backup must be consistent, and the selected partition UUID/type/bounds must
match the private profile. It opens the clean journal/orphan engine without
mount activation, then invokes the existing internal structural account checks.
It does not fake trusted-root flags or call the public namespace-admission API.
It asserts no published mount, no namespace fence and complete engine cleanup.
Every device write/flush callback counts the attempt and returns failure; every
case asserts both counters remain zero. The host registry stubs do not implement
kernel block-registry lifetime or scheduler behavior.

## Exact verification

Run in WSL Ubuntu-24.04 from `/mnt/c/Sources/FortressOS`:

```sh
make -f tests/pkg_grant_candidate.mk test-pkg-enroll-geometry
```

Final retained campaign:
`.codex-remote-attachments/pkg-enroll-geometry-20261024-091750-757768/`.
Its manifest records exact compiler/tool commands, copied dependency hashes,
binary and disk hashes, per-case diagnostics and separate preservation results.

- Actual-code host verification: **PASS**, 16 cases: one accepted canonical
  layout and 15 refusals. All cases attempted zero writes and zero flushes.
- Refusals: filesystem read error; independently checksummed `/etc` and private
  directories each one slot short; ordinary unreserved directory layouts;
  wrong `/etc` mode, root owner, shadow mode and private-directory owner;
  account hardlink; missing shadow; bad directory checksum; damaged primary
  header, backup header and primary table; valid foreign partition identity.
- Linux oracle: **PASS**, `e2fsck -fn` before and after inspection, five native
  inode metadata inspections and three byte-exact account-file dumps.
- Preservation: **PASS**, complete input hashes for pre-existing source/test/
  script/document files, Makefile, AGENTS.md, PROTECTED.md and shared bin files
  remain unchanged by the campaign. All 16 complete disk images remain
  byte-identical after checking; every unrelated sentinel partition matches.
- **Boot acceptance: NOT RUN.** No kernel, firmware or physical-device evidence
  is inferred from these host and preservation results.

Retained failures (each independently reports preservation PASS):

- `pkg-enroll-geometry-20261024-091457-381882`: dependency discovery failed from
  missing `src/lib` include path.
- `pkg-enroll-geometry-20261024-091525-253767`: compile failed from missing DAC
  definition and incorrect block-registry stub return type.
- `pkg-enroll-geometry-20261024-091554-521078`: link failed from missing permission
  units and unregister stub. The harness now links permission/credential/account
  codec units and discards unused retained host-test code with section GC.
- `pkg-enroll-geometry-20261024-091630-857987`: intermediate PASS, 13 cases,
  superseded by the added read-error and explicit capacity-boundary cases.

## Remaining native integration

Next bind nonzero inode identities, actual maps and account-file hashes into
canonical initial IDLE records and the signed descriptor; independently verify
those bytes from the actual filesystem before publishing the TEST selector.
Then deliver selector/policy as separate private boot modules, measure actual
kernel/bootstrap files, and validate the selected root in the projected-journal
preview before recovery writes. The separate modules avoid a selector/bootstrap
hash cycle. Native Ring 3 status/recover and BIOS/UEFI acceptance follow that
integration; no new boot support is claimed here.

This work does not authenticate root records, parse credential contents in the
kernel, certify resource escrow, replay a pending journal, exercise publication
cuts or establish cross-media atomicity. The selected filesystem stays detached.
Production trust provisioning, installation, release-media builds, pushes and
physical-device writes remain disabled. The original production plan remains
unmodified (SHA-256
`f1e8a769becaac55509a82a1633fda9c6057167588becab8c9cba91521fb1017`).
