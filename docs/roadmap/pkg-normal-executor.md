# PKG simplification — 2026-10-30

The device-enrollment approach was removed at the user's request. The current
scope is ordinary signed repository packages. See the
[current package plan](../plans/PKG_PRODUCTION_DESKTOP.md).

Removed components include the two package-specific installed-root boot modes,
five-module authority/selector/record-key capture, key-medium separation checks,
public authority/selector codecs and producers, POC root manifest/capsule/record
and bundle tools, projected enrolled-root constructor/backend, account-generation
observation, exclusion guards and tests specific to those removed paths.

The original FPK format, builder, publisher, signature/artifact verification,
planner/transaction engine, normal syscall/session lifetime integration and
bounded descriptor ingestion remain. Existing engine fixture suites remain test
support; none is introduced into release media as a normal admission adapter.
The ordinary installed-root admission path is restored. Bootstrap `/run/pkg`
availability is retained for older installed roots without a current `/bin/pkg`.

Downloads use ordinary root-owned URL and public-key configuration. They verify
the catalog before requesting its hash-addressed object, then verify the complete
FPK and platform/base compatibility. No device secret, independent boot selector
or laptop identity is required. Downloading still does not activate a package.
The replacement simple installation binding remains to be implemented; the
legacy engine remains dormant in normal builds pending that binding.

Obsolete enrollment build directories and generated test-image/output folders
were removed. They are no longer retained as evidence for the current design.
The fresh `test-pkg-fetch-host` run is recorded under
`.codex-remote-attachments/pkg-fetch-20261030-080733-838889`: PASS, ASan/UBSan,
actual frontend/verifiers with mocked file/network delivery, source preservation,
wrong-key/platform and invalid-signature/integrity rejection. This is download
verification evidence, not physical install or update acceptance.
