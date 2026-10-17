# FortressOS crypto primitives

Monocypher **4.0.3**, tag `4.0.3`, Git commit
`ab2b16dd619ad5f6979a4fbe69cfa324a6fcc35f`, official release archive
<https://monocypher.org/download/monocypher-4.0.3.tar.gz>.

Archive SHA-256:
`8cc9bc341a66249016db9bd70e9142d8d0aef9945973744b1ac05dbc55d8ee66`.
Individual file SHA-256 values are in [upstream.json](upstream.json); every host
test verifies them. Sources under `upstream/` and `LICENCE.md` are byte-for-byte
copies of the official archive. Upstream's release tool replaces `__git__` with
`4.0.3` in the first comment line; this is the only difference from the four
tagged Git source/header files. The archived comparison is retained separately.
`namespace.h` applies compile-time symbol/type prefixes; upstream files are not
patched. The complete dual license is reproduced verbatim; FortressOS chooses
BSD-2-Clause. Copyright and attribution remain in every source. Binary media
carry `/docs/monocypher-licence.txt`.

Use [crypto.h](../../include/crypto.h), **not** the private vendor headers.
`make all` builds `build/crypto/libfortresscrypto.a` with scalar x86-64
freestanding flags, no red zone/SIMD/heap/LTO and `-Os`. Link after application
objects. The archive contains the wrapper plus reachable namespaced primitives;
unexposed Argon2, Elligator, Blake2b and optional HKDF code is discarded by the
partial linker. A separate archive member supplies existing project memory
functions if the application needs them. Nothing is linked into the kernel,
and no primitive syscall is added. Existing digest and DRBG code is unchanged.

SHA-256 wraps the existing `root_digest.c`/`user/tools/digest.c` codec. Monocypher
core supplies curves, ChaCha20, Poly1305 and wiping; the optional
`monocypher-ed25519` module supplies SHA-512 and **standard RFC8032 Ed25519**.
Default upstream `crypto_eddsa_*` uses Blake2b and is not the selected signature
scheme. IETF AEAD uses `crypto_aead_init_ietf` plus one read/write with a 12-byte
nonce, not upstream's one-shot 24-byte XChaCha API. This is RFC8439 AEAD, not
OpenSSH packet encryption.

Random `*_keygen` APIs take only output buffers and obtain a fresh 32-byte seed
through `SYS_GETRANDOM`. Explicit `*_from_seed` functions are deterministic and
never call the OS. This resolves the task's contradictory seed-input/randomness
signatures without silently ignoring a supplied seed. All local generated seeds
are erased, and errors/short random reads publish no key material.

No hosted dependencies, dynamic allocation or mutable globals. Caller-owned
streaming contexts are consumed/wiped by finalization; lengths and aliases are
checked before output. Authenticated open verifies before decrypting and preserves
output on failure. X25519 rejects zero DH output. Ed25519 verification adds
canonical y encodings and low-order A/R rejection before the upstream cofactored
RFC8032 equation. Full prime-subgroup membership validation is not claimed.

The strict per-function stack gate is currently **pending the requested limit
decision**: GCC reports 1,104 bytes for unchanged upstream
`mono_crypto_eddsa_check_equation`, exceeding the requested <1,024-byte limit.
All other reachable frames are below 1 KiB. A conservative generated call-graph
sum is 1,880 bytes at `crypto_ed25519_verify` (library only, excluding caller and
OS syscall/interrupt stacks). `test-crypto-host` deliberately fails this gate
until the requirement is resolved; passing vectors do not override it.

Security properties are upstream claims, not independently re-proved/audited by
FortressOS. Source and optimized scalar code receive a bounded integration review;
tests and timing observations are not constant-time proofs. See the current
[upstream bug notes](https://monocypher.org/bugs), including the comparison-result
branch caveat on some compilers. Fixed-length comparison results are public in
this API; do not combine them to hide a secret predicate. Verification branches
on public signatures/keys. Firmware, caches, speculative execution, register
residue, physical RAM erasure and malicious callers in the same address space
are not solved by this library. Caller owns output key lifetimes and nonce
uniqueness. Wrappers wipe owned RAM scratch; upstream owns its internal wiping.
Source-review finding: optional upstream `hash_reduce()` does not explicitly
wipe its local 64-byte `hash` after scalar reduction. During signing this holds
the nonce hash before reduction. Wrapping the high-level signer cannot erase
that callee's scratch. The vendor code is retained unchanged as requested;
wrapper wipe-hook tests do not establish complete upstream-stack erasure.
This finding must be resolved or explicitly accepted before claiming that all
secret temporary state is wiped. No silent upstream patch is applied.

Tests: `make test-crypto-host`, `make test-crypto`. The latter builds an isolated
source copy and test-only `/bin/crypto-probe`; normal release initramfs/ISO/IMG
never include it. Fixtures and raw test entropy are not release inputs. The
library is not a TLS, signed-package or secure-boot implementation; passing
vectors does not qualify those integrations. No Phase 3 host provisioning occurs.
