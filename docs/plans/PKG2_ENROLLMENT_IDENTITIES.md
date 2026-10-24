# Private actual-filesystem identity gate

2026-10-24. Continues the private geometry gate committed in `94c63fc`.
No production source, boot behavior, ABI or trust provisioning changes.

The fresh-fixture producer now has an explicit `identities=True` profile. It
assigns nonzero generations to protected inodes and reseals directory CRC32C
checksums with the new generations. It captures pathname identities and maps
before changing generations, updates inodes by number, and reseals directories
before any subsequent pathname traversal. The default geometry profile remains
available without generation assignment.

`scripts/pkg_enroll_identity.py` independently reads raw inode tables, inode and
block bitmaps, inline extent headers and file bytes. For twenty fixed protected
paths it requires root ownership, nonzero generation, dense initialized depth-0
extents, one through four extents, allocated bitmap bits, correct sector counts,
single links for regular files, bounded geometry and no shared data blocks.
It emits a private unsigned inventory and JSON content hashes. The first sixteen
128-byte rows match the enrollment locator representation; four further rows
cover the descriptor and the three credential files. This inventory is a host
witness, **not signed enrollment, trusted policy or a published disk ABI**.

The actual-code C checker independently resolves all twenty paths with EXT4
lookup, validates inode checksums through the native reader, compares nonzero
generations, modes, owner IDs, sizes and every physical extent, and rejects
noncanonical witness headers/reserved bytes. The existing native ownership and
account-directory checks still run first. No inode identity or map is inferred
solely from the unsigned witness. The fixture stays detached and no namespace
fence or account service is activated.

## Exact evidence

```sh
wsl -d Ubuntu-24.04 -- make -f tests/pkg_grant_candidate.mk test-pkg-enroll-identity
```

Final campaign:
`.codex-remote-attachments/pkg-enroll-identity-20261024-092259-522641/`.

- ASan/UBSan actual GPT/EXT4/JBD2/account and identity checks: **65/65 PASS**.
  The sixteen structural cases remain covered; forty further cases mutate the
  generation or physical map of each of twenty rows; eight cases mutate witness
  size/version/count, reserved bytes, role, mode, file size and extent-tail bytes;
  one positive case compares all twenty actual identities.
- Every case asserts **zero write attempts and zero flush attempts**, engine
  cleanup, no published mount and no namespace fence. The canonical identity
  witness and all sixteen disk images remain byte-identical after checking.
- Linux oracle: **PASS**, initial/final `e2fsck -fn`, five metadata inspections
  and three exact account-file dumps. Directory checksums using the newly set
  generations are accepted by both Linux and FortressOS.
- Preservation: **PASS** for the campaign's pre-existing source, test, script,
  document, Makefile, instruction and shared-bin input inventory. Copied compiler
  inputs and exact commands are retained in the manifest. Runtime acceptance
  and preservation are separate fields.
- **Boot acceptance: NOT RUN.** The checker runs in the host process; no native
  boot delivery or recovery-write admission is established by these results.

After the identity changes, the original profile was rerun with
`wsl -d Ubuntu-24.04 -- make -f tests/pkg_grant_candidate.mk test-pkg-enroll-geometry`.
Campaign `pkg-enroll-geometry-20261024-124121-565661` reports **16/16 runtime
PASS** and **preservation PASS**. This is a separate compatibility regression
check, not additional boot evidence. A concurrent change to
`assets/ssl/ca-certificates.sha256` was observed before this rerun and remains
untouched and excluded from the package commit; that asset is not part of this
runner's source/media preservation inventory.

Retained failure:
`.codex-remote-attachments/pkg-enroll-identity-20261024-092151-687751/`.
The fixture initially changed directory generations before collecting all path
identities, so Linux debugfs correctly refused later traversal with stale
directory checksums. This producer-ordering failure remains FAIL; preservation
independently passed. The corrected producer captures identities/maps first and
uses inode-number updates before resealing directories. Validation was not
weakened to accept stale checksums.

The account content hashes in the JSON are observations, not authenticated
lineage. Signing canonical initial IDLE records using these exact native maps,
checking their readback before selector publication, projected-journal admission,
separate boot-module delivery and native boot tests remain required. No pending
journal, allocation escrow, cross-media crash atomicity or physical power-loss
safety is claimed. All operational prohibitions remain in force.
