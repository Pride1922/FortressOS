# App package builder — Stage 1 (2026-10-30)

Starting checkout: `main`, `9df5b53d72bce7a96551ac3776d5fc49ffe9fd41`.
The existing local edit to `docs/plans/PKG_PRODUCTION_DESKTOP.md` was preserved.
Stage 1 changes host packaging/publication only. Kernel, syscall, installation,
trust/admission and boot behavior are unchanged.

## Delivery

- `scripts/pkg_build.py build SPEC --out ARTIFACT` accepts strict JSON and
  resolves local source paths relative to it. It rejects links, special files,
  set-ID source modes, unsupported paths and invalid metadata. The deterministic
  encoder retains FPK v1's single executable and bounded `doc/` file set.
- Build and `inspect ARTIFACT` use a temporary host adapter around the actual
  shipped C verifier (including hashes, gzip/USTAR, manifest and static ELF).
  Successful inspection does not authenticate a catalog or authorize installation.
- The desktop builder keeps its CLI/API and wraps the generic encoder. The same
  desktop input produces bytes identical to the checkpoint implementation.
- `pkg_repository.py --retain-existing` authenticates the previous catalog and
  retains other offered objects while replacing the named package. It refuses
  lower package builds, equal-build substitution and concurrent catalog changes.
  The existing explicit full-catalog mode and on-wire formats remain available.
- `examples/pkg/rot13` builds a real freestanding stdin/stdout filter in releases
  1.0/build 1 and 1.1/build 2; the second release adds `--help`. Neither is added
  to the base initramfs. Checked-in JSON compatibility is illustrative only.
- [Ubuntu instructions](../pkg-repository-ubuntu.md) cover approved metadata,
  build, inspection, external signing seeds, retained catalog publication and
  object-first deployment to the existing document root.

## Host evidence

Run in WSL Ubuntu-24.04 from `/mnt/c/Sources/FortressOS`:

```sh
make test-pkg-build
python3 scripts/test_desktop_package.py
make bin/fortress.elf build/pkg/pkg.elf
python3 scripts/pkg_build.py inspect build/pkg-rot13/v2.fpk
```

All passed. The focused test builds both real static ELFs, proves deterministic
FPK output, checks the existing C verifier with ASan/UBSan, and exercises the
actual filter logic with ASan/UBSan through a host I/O adapter over all byte
values and multiple buffers. It checks version/help behavior, malformed ELF,
corruption, paths, duplicate files, ancestor collisions, bad metadata, links,
set-ID input and FIFO refusal. Publisher tests use a fixed **public TEST vector**
seed only in memory; no key files are generated or provisioned. They verify
retention of an unrelated offer, invalid-signature refusal, rollback and
substitution rejection, stale preparation refusal and failure before catalog
activation followed by successful retry. The prior catalog remains byte-identical
at the simulated cut. Desktop regression passes its sanitizer verifier, and an
additional direct comparison with `git show 9df5b53:scripts/build_desktop_package.py`
confirms byte-identical output for the same compositor and release metadata.

Unsigned illustrative artifacts are in `build/pkg-rot13/v1.fpk` and `v2.fpk`.
No live publication, publisher secret generation/provisioning, enrollment, QEMU,
physical disk access, commits or pushes were performed. Host verification does
not establish FortressOS launch, installation, upgrade or Dell acceptance;
those remain outside Stage 1.
