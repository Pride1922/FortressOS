# Stage 2 ordinary-root install fault and error reporting

2026-10-31, local changes based on 83ae2b9.

The ordinary install adapter decoded install.v2 and called px_encode before
px_prepare initialized the global transaction context. px_encode reads that
context to serialize filesystem/layout identity. This fault was reachable on a
first install as well as a repeated install. The shortcut also held an 8192-byte
stack array and could accept an identical artifact without checking the current
catalog authorization or installed files.

Remove the shortcut and retain the existing verified PI_NOOP path. Preserve
platform hashes whose first byte is zero when serializing a cold installed
record; a zero first byte is not an absent platform identity.

The CLI prints backend/installation progress, reports the kernel error with
specific I/O, space, memory, verification and busy explanations, and recommends
status inspection before retrying. Both source descriptors and the retained
session are released on returned errors. A successful unchanged transaction
sequence reports `package is already installed rot13 v 1.0` using bounded manifest
display metadata; only kernel verification/result authorizes success.

Verification commands:

- `make test-pkg-ordinary-install-host`: actual ordinary callback, VFS, EXT4 and
  JBD2 on a new disposable regular-file image under ASan/UBSan. First install,
  fresh-context identical reinstall with zero additional writes, invalid catalog
  signature/artifact refusal, and injected read I/O error return. The fixture
  platform starts with zero. Host clock/credentials/disk are test controls.
- `make test-pkg-stage2-host`: CLI/download tests, including exact already-installed
  text, and injected I/O, no-space, memory and busy failures with fd/session cleanup.
  Its SYS_PKGCTL is mocked; this suite alone does not establish kernel installation.
- `make -j4`: normal kernel, CLI and release media build.

No Dell or QEMU runtime acceptance is claimed by these host checks. The current
production desktop plan retains Dell-first acceptance. No physical disks, trust
provisioning or protected synchronization contracts are changed.

Results: PASS for the ordinary callback ASan/UBSan suite, Stage 2 CLI 15/15,
`python3 scripts/test_pkg_system_host.py` (runtime and source preservation), and
`make -j4`. The normal callback frame is 192 bytes (previously 9712 bytes).
An isolated rebuild of 83ae2b9 with the same callback test reproduces ASan/UBSan
null-context access in px_encode, called from ext4_pkg_install_source.

Local artifacts:

- `.codex-remote-attachments/pkg-ordinary-h1ubfupx/`: fixed callback test binary,
  disposable fixture and `before-fix.log` with the original sanitizer trace.
- `.codex-remote-attachments/pkg-system-host-20261031-080701-833235/manifest.json`:
  transport/owner/frontend runtime and preservation PASS.
- `bin/fortress.img` and `bin/fortress.iso`: rebuilt normal media.
