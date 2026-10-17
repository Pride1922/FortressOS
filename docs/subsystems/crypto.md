# Cryptographic primitives — SSH Phase 2 / #50

Implementation and functional tests are present. **Closure is pending the strict
stack-limit decision**: unchanged Monocypher's verifier has a measured 1,104-byte
frame, over the task's <1 KiB bound. It was proposed to allow only this verifier
exception with a 1,280-byte ceiling; that exception is not yet authorized.
The automated host target intentionally reports failure for this limit.
An additional source-review finding remains: upstream optional `hash_reduce()`
does not explicitly erase its 64-byte local hash, including the signing nonce
hash before scalar reduction. High-level wrappers cannot erase callee scratch.
The vendor source is unpatched; wipe observers cover wrapper-owned storage,
not complete upstream-stack erasure. A full temporary-erasure claim is withheld.

## Dependency and interfaces

Pinned Monocypher 4.0.3, commit
`ab2b16dd619ad5f6979a4fbe69cfa324a6fcc35f`, BSD-2-Clause chosen from its verbatim
dual license. Archive SHA-256
`8cc9bc341a66249016db9bd70e9142d8d0aef9945973744b1ac05dbc55d8ee66`;
individual identities are in `src/lib/crypto/upstream.json`. Release version
stamping accounts for the first-comment-line difference from Git; no FortressOS
source patch. See [integration notes](../../src/lib/crypto/README.md).

Public API: [src/include/crypto.h](../../src/include/crypto.h). All returns are
zero on success/equality/validity, negative on failure; verification is not a
0/1 truth-value convention. Accessible caller-owned buffers are required; this
library cannot validate memory mappings. Invalid pointer/length/alias conditions
return FortressOS `SYSCALL_EINVAL` before publication. Actual keygen propagates
`SYS_GETRANDOM` errors; a short positive result maps to `SYSCALL_EIO`.

| Primitive | Public functions | Source / limits |
| --- | --- | --- |
| SHA-256 | `crypto_sha256`, `crypto_sha256_init/update/final` | existing digest codec, unchanged |
| SHA-512 | `crypto_sha512`, `crypto_sha512_init/update/final` | optional Monocypher module |
| HMAC-SHA-256 | `crypto_hmac_sha256` | RFC2104 over existing streaming SHA-256, keys >64 bytes hashed first |
| Ed25519 | `crypto_ed25519_from_seed`, `crypto_ed25519_keygen`, `crypto_ed25519_sign`, `crypto_ed25519_verify` | pure RFC8032 SHA-512 Ed25519, no ctx/ph/Ed448 |
| X25519 | `crypto_x25519_from_seed`, `crypto_x25519_keygen`, `crypto_x25519` | scalar clamping, low-order/all-zero DH refusal |
| ChaCha20-Poly1305 | `crypto_aead_lock`, `crypto_aead_unlock` | RFC8439, 32-byte key, 12-byte nonce, 16-byte tag |
| memory | `crypto_verify32`, `crypto_wipe` | fixed-size comparison, volatile erasure |

SHA/HMAC/signature message lengths are capped at `UINT64_MAX/8-256` and must not
wrap pointer arithmetic. AEAD limits text to `(2^32-1)*64` bytes, so counter 1
never crosses the IETF 32-bit block space; counter zero supplies Poly1305.
Zero-length data permits NULL; fixed-size key/output pointers do not. Read-only
inputs may alias; writable outputs must be disjoint from inputs/each other,
except exact in-place AEAD. Partial overlap is rejected. There is no unauthenticated
plaintext publication, even in-place on a bad tag. Streaming input/output may
not overlap its context. Finalization wipes/consumes a context; bad updates
preserve it. Failed output buffers are preserved, rather than silently zeroed.

No heap, lock, scheduler call or writable library global; caller-owned states
make operations independent across processes/CPUs. Only random keygen calls the
OS. Deterministic `from_seed` APIs exist for known inputs and explicit use, not
as a failure fallback. No new crypto syscall or SSH framing/auth/transport code.

## Evidence

Evidence root: `.codex-remote-attachments/crypto-phase2/`.

- `host.log`: actual wrappers/vendor at GCC -O1, ASan/UBSan. RFC6234 abc and
  million-a plus empty/single-byte and streaming lifecycle; all seven RFC4231
  cases (case 5's specified 128-bit prefix); all **five pure-Ed25519** RFC8032
  §7.1 vectors (including 1023-byte and SHA(abc) messages); RFC7748 DH and
  iterations 1/1000; RFC8439 §2.8.2 exact ciphertext/tag. The task's counts of
  ten RFC4231 and seven pure-Ed25519 vectors do not match those RFCs. Different
  Ed25519ctx/ph algorithms are not silently treated as pure Ed25519.
- Negative cases: 320 Ed signature byte mutations, wrong public key/message,
  noncanonical S/y and low-order identity, 14 low-order/high-bit X25519
  encodings, all-byte AEAD tag/ciphertext mutation, wrong key/nonce/AD,
  no-output-on-failure canaries, partial overlap, NULL and length/wrap limits,
  generated-key entropy EIO/EAGAIN/short-read failure and seed-wipe observers.
- `host.log` also contains 96 fixed-seed randomized differential cases using
  independent Python hashlib/HMAC and cryptography Ed25519/X25519/AEAD across
  zero/block-boundary/64 KiB messages, plus 1 MiB hashes. No OpenSSL/BoringSSL
  vector source files are imported. Independent behavior is supporting evidence,
  not an independent security audit.
- `upstream-test.log`: unmodified official archive `make test` passes;
  `archive-git-comparison.json` records exact release-vs-Git reconciliation.
- `stack.json`: reachable per-function `.su` reports, bounded outgoing argument
  stack areas, conservative direct call/tailcall graph sums. Only the unmodified
  verifier exceeds 1 KiB. `library.disassembly.txt` records supported GCC scalar
  optimized instructions. No dynamic/unbounded stack, SIMD, writable global or
  allocation is part of the public archive. Unused upstream algorithms are
  removed by the partial linker.
- `qemu.log`: **4/4** disk-free q35 TCG BIOS/UEFI SMP=1 ready/absent boots pass.
  Campaign `.codex-remote-attachments/crypto-qemu-20261017-071327/` retains
  test-only source/media, argv, OVMF vars, UART/stderr, manifest and summaries.
  Ring 3 pure vectors, fresh random sign/verify/keygen, DH, AEAD and failure
  preservation pass. Existing getrandom probe and `echo abc | sha256sum` pass.
  Source and shared release media hashes remain unchanged during the campaign.
- `regression-host.log`: existing time and credential-syscall host targets pass.
  `getrandom.log`: full **14/14** BIOS/UEFI × SMP=1/4/8 present/absent plus
  RDRAND-only SMP=4 regression passes, campaign `random-20261017-071556`.
  `release-build.log`: `make -j8 all` passes, including IMG GPT/ESP/ext2 audits.
  `release.json`: upstream/initial-source identities, scalar-only and no mutable
  library globals, no host test hooks, no crypto probe in ELF/initramfs/ISO/IMG.
  Existing digest/DRBG/installer-seed/TCP/time/syscall sources are byte-identical
  to `before.json`. No protected production code or existing SHA-256 is changed
  by Phase 2.

## Security claim boundary

Upstream supplies constant-time secret operations; FortressOS does not
independently re-verify that claim. A bounded source and optimized-code review
checks integration and scalar compiler output; tests/timing are not proofs.
Version 4.0.3 fixes the earlier signing timing leak. Upstream still documents
a comparison-result-dependent branch on some compiler/platform combinations;
this API treats comparison/verification results as public, not composable hidden
predicates. Public-data verification may branch. No claim covers the whole OS,
microarchitecture, physical entropy, secret registers or persistent-storage erasure.

The library is a set of primitives, **not TLS, signed packages or secure boot**.
Passing vectors does not qualify any of those integrations. It also is not SSH:
RFC8439 AEAD is different from OpenSSH's packet construction. Phase 3 is not
started; `SYS_FSTAT` and host-key publication remain separate work.
