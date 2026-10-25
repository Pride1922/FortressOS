# Private bounded filesystem executor

This is a copied-source Gate 3 candidate. It does not enable installation in the
default kernel or `/bin/pkg`. Production provisioning, release media, pushes and
physical writes remain disabled. The original production integration plan is
unchanged.

`tests/pkg_install_fs.inc` executes verified `pi_plan` values through actual
mounted EXT4/VFS callbacks and the semantic JBD2 commit boundary. The overlay
changes only a retained copy. The independent Python fixture seeds an empty
authenticated ledger; it does not pre-sign installed endpoints. The private
`pkg_mutation_cli` accepts a package name resolved by its trusted callback.

## Supported bounds and authority

One optional package slot per mount, one executable of at most 4 MiB, zero to
15 documentation files, at most 64 derived parent directories, 4096-byte blocks
and at most four inline extents per member. No upgrades, multiple installed
packages or arbitrary command-line paths. Source/catalog and expanded archive
are copied and verified before target mutation. Total preallocated executor
memory is checked against trusted policy and a 32 MiB ceiling. Journal, orphan,
cache-node and rename-publication credits are bounded.

Trusted admission supplies the filesystem UUID, fixed inode/generation anchors,
state-file physical mapping, platform/base names, signed catalog policy and an
external minimum ledger sequence/hash. The mutable 8192-byte ledger uses a
separate kernel-only HMAC-SHA256 key, not an Ed25519 signing key. Tests generate
that key outside the repository, retain only its fingerprint and remove the key
after the campaign. Production key custody/provisioning is not implemented or
authorized. An attacker possessing that key can forge the local ledger. Whole
disk rollback still requires advancement of the external minimum; an on-disk
MAC alone cannot detect rollback of the entire disk.

## Durable operations

The first transaction reserves the complete plan and advances local build,
root and catalog floors. Every filesystem transaction records its inode claims,
actual physical charges and remaining reservation in the same journal commit.
Ordinary allocation preserves the remaining package reservation and the
64-block/16-inode account allowance. Fixed account-role allocation may consume
its own allowance but cannot spend the package reservation. Protected ancestors,
members and directories cannot be modified by ordinary filesystem commits.

BUILD creates and fills private members and an independent authoritative copy of
the executable. Documentation publication precedes the COMMIT decision; recovery
before that decision removes only authenticated owned objects. COMMIT rolls
forward executable publication. RETIRE releases private evidence after installed
ownership is durable. Removal records DELETE before hiding public members,
truncating private objects and retiring their exact inode/generation claims.
All removal paths retain the trust/build floors. Open members defer deletion;
their bytes remain intact. Freed capacity is refunded only after actual bitmap
release. Repeated completed operations perform no package writes.

Each bounded storage step rechecks the embedding's authority callback outside
filesystem exclusion. Orphan loops yield after one semantic commit. A serialized
operation credit and atomic actor identity distinguish typed package commits
from concurrent ordinary writers. The embedding must retain the owner and mount
until all calls and cached-node references drain; destroying a live context is
not supported.

## Validation and delivery boundary

Run `python3 scripts/test_pkg_install_fs.py` inside WSL Ubuntu-24.04. It compiles
an isolated source copy with strict warnings and ASan/UBSan, uses fresh regular
files, retains command logs and input hashes, and reports runtime and repository
preservation separately. It tests actual install/remove/reinstall, private CLI
dispatch, open-file bytes, output failure, forged ledger/key/floor refusal,
authority loss, interruption recovery and low-space refusal. Linux e2fsck and
an independent Python content/metadata/ledger oracle check completed outputs.

These tests do not establish boot acceptance, actual scheduler teardown, native
account-service concurrency, physical power-loss safety or production key
custody. The new executor still requires integration with the retained kernel
owner/request path and installed-root admission, followed by the Gate 4 Ring 3
BIOS/UEFI campaign. Existing status/recover boot evidence does not cover this
new mutation executor. Public mutation commands must remain disabled pending
that integration and acceptance review.
