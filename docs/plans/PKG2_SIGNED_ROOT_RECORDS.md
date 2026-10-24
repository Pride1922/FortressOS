# Signed records on a fresh TEST root

2026-10-24. Implements the next private offline enrollment prerequisite after
the actual-filesystem identity gate. Production sources, default dispatch, boot
behavior and the original integration plan are unchanged.

## Implemented

`scripts/pkg_enroll_records.py` builds canonical initial IDLE records from the
fresh fixture's actual nonzero inode identities and dense physical maps. It
creates the marker, empty ownership table, IDLE grant, EMPTY origin, closed
account-generation state, empty package state, signed catalog/trust state and
signed descriptor. Credential hashes and sizes describe the actual root-only
passwd/group/shadow files, not synthetic length witnesses. The descriptor's
first sixteen locators are the independently collected actual filesystem maps.

Three disjoint TEST roles each require three external private seed files; two
signers form each threshold. Seeds must be private regular files outside the
repository. The test runner supplies explicitly public fixture seeds in a
temporary directory; it retains public authority metadata, not seed files.
The future demo catalog entry authorizes no installed application. The root
authority JSON is an externally pinned TEST audit input, not a production root
delegation loader.

The writer accepts only the matching fresh regular-file fixture with untouched
placeholder record contents. It overwrites logical content through the retained
maps, fsyncs each record and then the root file, and does no allocation or
metadata mutation. It refuses a second enrollment before the first write.
This is offline preparation of a new disposable filesystem, not installation
or an API for modifying an existing installed root.

The read-only actual-code host adapter reconstructs the bundle by native EXT4
pathname lookup and physical reads. It compares signed descriptor locators to
the native identity expectations already checked, compares descriptor geometry
and filesystem UUID to the actual superblock, and hashes the actual credential
files against the account record. The selected GPT partition must match the
fixed private profile. The independent C selector reader then verifies the
native-read bundle's signatures, role thresholds, initial-state semantics,
hashes and external policy. Both checks precede TEST publication.

Only the explicitly named candidate selector is retained in preflight artifacts.
The committed selector appears in the new private publication directory after
native readback and cryptographic validation. Publication uses the existing
root-before-selector fsync/readback/rename sequence. It copies the verified
bundle as an audit artifact; this does not itself deliver a kernel boot module.

## Exact tests and retained evidence

From the workspace, with WSL Ubuntu-24.04:

```sh
wsl -d Ubuntu-24.04 -- make -f tests/pkg_grant_candidate.mk test-pkg-enroll-records
wsl -d Ubuntu-24.04 -- make -f tests/pkg_grant_candidate.mk test-pkg-enroll-identity
```

Final signed-record campaign:
`.codex-remote-attachments/pkg-enroll-records-20261024-125427-391488/`.

- Actual-code ASan/UBSan native readback and cryptographic verification **PASS**.
  The complete native-read bundle exactly equals the prepared signed bundle.
  Native disk checks attempt zero writes and zero flushes and leave no published
  mount, namespace fence or retained engine allocation.
- Eleven corruption cases **PASS**: descriptor and all seven other records,
  plus each credential file. Corrupt descriptors/credential lineage refuse in
  the native reader; the other corrupted records refuse in the independent
  cryptographic reader. Every inspected disk remains byte-identical.
- Nine root fsync-boundary interruptions **PASS**. Actual partial root snapshots
  are retained and inspected by the native reader. Partial coherent bundles
  refuse cryptographic validation; cuts after the final record yield a valid
  complete bundle, but still refuse admission because no selector is committed.
- Fifteen selector-publication interruptions **PASS**. Cuts before rename refuse;
  the three cuts at/after rename contain fully verified artifacts. The actual
  enrolled disk remains unchanged throughout publication checks.
- Linux oracle **PASS**: `e2fsck -fn`, eight byte-exact signed-record dumps and
  three byte-exact credential dumps. A separately constructed whole-image oracle
  confirms that only logical record content changed: all metadata, allocation
  maps, padding and unrelated bytes remain exact.
- Supplemental repeat-enrollment refusal **PASS**, recorded in
  `repeat-enrollment-refusal.json`: no writer events and unchanged root hash.
- Runtime result **PASS** and source/media preservation **PASS**, independently
  recorded. Compiler dependency copies and hashes reconcile. The manifest
  retains exact compiler, native-reader, cryptographic-reader and Linux commands;
  publication event ordering is separately recorded.
- **Boot acceptance: NOT RUN.** Kernel/bootstrap hashes are clearly marked
  synthetic host TEST identities. No real kernel/bootstrap measurement or native
  boot-module delivery is inferred from host acceptance.

Regression campaign:
`.codex-remote-attachments/pkg-enroll-identity-20261024-125611-173722/`:
**65/65 host cases PASS**, Linux audits PASS, preservation PASS. This checks
compatibility of the original structural/identity modes after adding bundle
export and credential hashing.

Intermediate campaign `pkg-enroll-records-20261024-125139-667223` remains a
**PASS** checkpoint. It predates native partial-root inspection and explicit
descriptor geometry/locator comparisons; the final campaign supersedes it.
No failed campaign occurred in this step, and no earlier failure was relabeled.

## Remaining boundary

This is a fixed 4096-byte-block, 512-byte-sector, 32 MiB root/64 MiB GPT TEST
profile with inline extents and an empty installed inventory. Policy transport
is the private host layout, not a frozen boot ABI. Broader geometry, key rotation,
real boot measurements and production authority selection remain separate work.

Interruption tests are deterministic host exceptions after fsync boundaries,
not sector-tear, hostile-host, cross-media power-loss or physical durability
proof. The private producer and readers assume exclusive host ownership of the
fixture. Complete root namespace/capacity certification, projected-journal
admission before replay, account-service authentication and root publication
remain native-integration obligations. No account mutation, package install or
remove is enabled. Installation, production provisioning, release-media builds,
pushes and physical-device writes remain disabled.
