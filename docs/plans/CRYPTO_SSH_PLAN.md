# Crypto library, SSH server and host identity

Updated 2026-10-17. **Phases 1–2 complete; Phase 3 host identity implemented
and verified within the evidence below. SSH service phases remain planned.**
Tracks [crypto #50](https://github.com/Pride1922/FortressOS/issues/50),
[SSH server #51](https://github.com/Pride1922/FortressOS/issues/51) and
[host keys #52](https://github.com/Pride1922/FortressOS/issues/52).

## 1. Goal and delivery boundary

Provide an opt-in Ring 3 `/sbin/sshd` reachable from Windows/Linux OpenSSH,
with a persistent Ed25519 host identity, public-key and password authentication,
and an isolated interactive shell running with the authenticated account's
credentials. Preserve CPU-0 network ownership and the existing permission,
account-generation, descriptor, scheduler and installed-root contracts.

The first release supports IPv4 TCP, one session channel per connection and
two simultaneous SSH connections. No SFTP/SCP subsystem, agent/TCP forwarding,
compression, X11, certificates or root remote login. These are explicit request
failures, not silently accepted features. SSH remains disabled by default until
provisioning and acceptance pass; enabling it is a separate persistent setting.
Live media defaults to SSH disabled, including its passwordless local account.

The dependency order is:

`contracts -> [secure randomness + crypto (#50)] -> host provisioning (#52)`

`contracts -> terminal/session prerequisites`

`crypto + host identity + session/auth prerequisites -> SSH (#51) -> acceptance`

Issue numbering is not implementation order: deployable SSH needs #52 before
#51 can close. A transport demo or a shell attached to pipes does not close an
issue promising interactive remote login.
Randomness and deterministic crypto tests can progress together. A DRBG using
library primitives cannot pass its final gate until those primitives are
verified; deterministic primitive tests do not require production entropy.

## 2. Findings from the current checkout

| Existing component | Reuse and missing proof |
| --- | --- |
| Network SMP Phases 0–4 | Ordinary AP socket calls and CPU-0 ownership are accepted. SSH uses the existing retained socket API; no protocol work enters the crypto library or a second network worker. |
| `src/net/net_tcp.c` boot-secret initialization | Hardware samples have conditional assurance; timer/MAC/RTC fallback inputs are not a CSPRNG. Do not reuse this secret or change the protected ISN policy for SSH. |
| `src/kernel/accounts.c`, `accounts_seed` | Bounded, privileged hardware-random adapter exists through `SYS_INSTALLSEED`. It is not a general readiness/reseed/randomness contract for unprivileged SSH workers. |
| `user/tools/digest.h`, `src/lib/root_digest.c` | SHA-256 implementations exist. The inspected tool digest API provides MD5/SHA-256, not the full requested crypto suite. Select a canonical crypto implementation without disrupting persisted password/checksum formats. |
| `user/tools/userdb.h`, `src/include/account_abi.h` | Bounded account parsing, SHA-256-crypt verification, wiping and generation revalidation exist. Parser scratch is process-local and non-reentrant. Use separate processes or owned contexts, not shared concurrent parser calls. |
| `src/include/creds.h`, `syscall_abi.h` | UID/GID/groups and drop-only capabilities exist. Credential publication must follow `CREDENTIAL_SNAPSHOT_PROTOCOL.md`. Spawn is explicit; do not assume POSIX fork exists. |
| `src/include/terminal.h`, `src/drivers/input.h` | The published terminal interface describes shared input and foreground-group state. Per-SSH-session PTY isolation has not been established; audit/implement it before interactive acceptance. |
| VFS/EXT4 and installer | Rename, global sync, metadata permissions and detached provisioning exist. Safe no-replace secret-file creation, durable publication and read-by-descriptor validation need explicit contracts; do not assume POSIX fsync/openat behavior. |
| Socket/user ABI | Absolute network deadlines and nanosleep exist. A general poll/select/nonblocking interface was not established by this audit; bridge design must prove duplex progress with available APIs or add a reviewed bounded wait interface. |

Read PROTECTED.md, AGENTS.md §4/§7/§9, net/shell/storage/SMP annexes, credentials,
account, VFS, thread and socket headers before implementation. The APIs below
are proposals, with syscall numbers deliberately unassigned. This planning task
does not itself implement changes to protected terminal, credential or boot
contracts. No physical writes or installer-gate changes are part of it.

## 3. Protocol profile and crypto choices

Prefer a small, reviewed portable upstream implementation over newly invented
curve arithmetic. Phase 0 selects an exact version/commit after checking
license, provenance, review history, freestanding C compatibility, stack use,
maintenance and negative-vector behavior. Preserve notices and record hashes.
Do not describe an imported component as independently audited by FortressOS.

Initial SSH profile:

| Layer | Proposed selection |
| --- | --- |
| KEX | `curve25519-sha256` |
| Host and user signature | `ssh-ed25519` |
| Cipher | `chacha20-poly1305@openssh.com` |
| Compression | `none` |
| Authentication | Ed25519 publickey first; password before #51 closure |

Implement the exact shared-secret/mpint and transcript rules of
[RFC 8731](https://www.rfc-editor.org/rfc/rfc8731.txt), including wrong-length
and all-zero shared-secret rejection. Ed25519 SSH encoding is specified by
[RFC 8709](https://www.rfc-editor.org/rfc/rfc8709).

**Generic AEAD is not the SSH packet construction.** The library must offer
RFC 8439 ChaCha20-Poly1305, while the SSH adapter implements the distinct
packet-length encryption, key split, nonce/counter and tag rules described by
[the SSH cipher draft](https://datatracker.ietf.org/doc/draft-ietf-sshm-chacha20-poly1305/)
and OpenSSH specifications. At planning time this reference is a working draft;
pin the exact text and deployed client behavior during Phase 0. Do not
advertise a generic RFC 8439 implementation as SSH-compatible.

Ed25519 also needs SHA-512. Use interoperable RFC 8032 Ed25519, not a similarly
named signature variant using another hash. HMAC-SHA-256 remains a library
deliverable; AEAD SSH does not gain an extra ad-hoc HMAC layer.

## 4. Phases and acceptance gates

### Phase 0 — contracts and dependency audit

Write exact randomness, key-publication, privilege separation, remote-session
ownership and duplex-I/O designs before network exposure. Trace shell/login
and terminal indirect calls; enumerate global input/output assumptions.
Define a bounded monitor/worker protocol using spawn, retained descriptors
and pipes rather than fork. Resolve whether a maintained SSH protocol core
can be ported within the freestanding/resource constraints; prefer reuse when
its dependency and security review is better than a new protocol engine.

Proposed initial limits: two total connections (including preauth), one channel
each, 36 KiB RX and TX record buffers per connection, five authentication
attempts, 10-second KEX deadline and 60-second authentication deadline. Define
global and per-source admission/CPU budgets, monotonic absolute timeouts and
finite teardown. SSH allocations have a separate measured ceiling, initially
512 KiB excluding documented process stacks/page tables; this is not permission
to enlarge the protected 576 KiB network-state budget or eight shared credits.
Retain enough sockets/TCBs for unrelated ping/DNS/HTTP; one listener plus two
accepted connections must not consume all eight TCP connections.

**Gate:** reviewed contracts, selected implementation/version/licenses,
resource ledger, error precedence and threat model. Treat unauthenticated
network data, usernames, public keys, terminal requests and file paths as hostile.

### Phase 1 — secure randomness prerequisite

**COMPLETE (2026-10-17):** actual-code host sanitizers/vectors/concurrency and
14/14 disk-free BIOS/UEFI boots pass; no physical entropy assessment claimed.
Implementation: `SYS_GETRANDOM` 77 and independent global HMAC-SHA-256 DRBG.
The explicit Phase 1 request supersedes Phase 0's provisional SHA-512,
continue-after-reseed-failure and nonzero-flags proposals. Fresh hardware reseed
is mandatory before every request; failures latch unavailable until reboot.
Contract, acceptance status and exact evidence: [Phase 1 report](../roadmap/ssh-phase1-random.md).

Design a bounded strong-random service with explicit readiness and error
behavior, reviewed hardware-source policy and a vetted DRBG/reseed design.
Document hardware-source trust and limits; instruction success alone is not
an independent entropy assessment. Define requests across CPUs, process spawn,
restart and exhaustion without repeated output streams. Keep internal state
unavailable to callers and erase temporary seeds. Hardware absence/failure
must fail closed for private keys, ephemeral secrets and protocol randomness;
no timestamp, MAC address, password or TCP-secret fallback.

Do not broaden `SYS_INSTALLSEED` or alter TCP ISNs incidentally. Decide the
general user API and any privileged monitor access separately. Injected seeds
are test-only and absent from release media.

**Gate:** actual-code sanitizer/concurrency and deterministic DRBG tests;
absent/failing/stalled source tests; bounded failure and no key generation on
failure; BIOS/UEFI SMP=1/4/8 readiness/lifecycle tests. QEMU entropy does not
establish physical source quality.

### Phase 2 — cryptographic library (#50)

**COMPLETE (2026-10-17, with explicitly approved verifier stack exception):**
actual-code ASan/UBSan RFC/negative/wipe gates, 96 independent differential
cases, the approved stack audit and four BIOS/UEFI ready/absent guest boots pass.

The user authorized the unchanged Monocypher verifier's measured stack exception
(1,104 bytes, 1,280-byte ceiling) and wrapper-owned signing hash glue on
2026-10-17. All other frames remain below 1 KiB. The new glue erases the nonce
hash and secret scratch while using unmodified upstream scalar/SHA-512
primitives; no vendor patch or new curve arithmetic. See
[crypto integration status and evidence](../subsystems/crypto.md).

Provide streaming SHA-256/SHA-512, HMAC-SHA-256, Ed25519 key generation/sign/
verify, X25519, ChaCha20, Poly1305 and RFC 8439 AEAD through small checked APIs.
Specify aliasing/length/overflow limits, failure publication, secure zeroing
and constant-time comparison. Do not expose unauthenticated plaintext from
AEAD open. Reject noncanonical/invalid signatures and low-order edge cases
according to the chosen algorithm's rules. Key-generation helpers obtain fresh
strong randomness; fixed seeds belong only to explicitly deterministic APIs
and test vectors.

Secret-dependent arithmetic/access paths require source and optimized-code
review on supported compilers/targets. Timing tests are supporting evidence,
not proof of constant-time execution; avoid a blanket claim for whole SSH
parsers or variable-length protocol encodings.

**Gate / #50 closure:** RFC 7748/8032/8439 and RFC 4231 HMAC vectors, boundary
and hostile vectors, mutation/fuzz tests, ASan/UBSan, independent differential
tests, failure-output/wiping checks and freestanding guest self-tests. Record
compiler flags, dependency identity and external-review scope. Passing vectors
does not by itself qualify TLS, signed packages or secure boot integrations.

### Phase 3 — persistent host identity (#52)

**Host-key implementation verified (2026-10-17; bounded scope):** [host identity design and resolved
storage interface](SSH_PHASE3_HOST_IDENTITY.md), based on handoff `d67ccf0`.
Bounded OpenSSH codec, root-only installed-root publication/load, explicit
provision/export command and installer v4 handoff are implemented. Host codec
and 595 mounted sector-atomic recovery cases pass, as do 18 installed BIOS/UEFI
guest boots at SMP=1/4/8. [Evidence and remaining boundaries](../roadmap/ssh-phase3-hostkeys.md).
Rotation requires a separate replacement design; daemon lifecycle, actual
installer guest provisioning and physical acceptance are not claimed. SSH remains disabled.

Generate a unique Ed25519 key on the target during authorized installation,
after the detached root is prepared. Never embed host private keys in build
templates, initramfs, ISO/IMG, source control or factory copies. Proposed path:
`/etc/ssh/ssh_host_ed25519_key`, root:root 0600, with a validated root-owned
directory not writable by other users. Export only the public key/fingerprint.
Use the bounded OpenSSH unencrypted single-key format for interoperability;
reject unsupported cipher/KDF/container variants rather than guessing.

Use checked creation, correct permissions from first visibility, full-write
handling, no-replace publication and supported durability barriers. Validate
ownership/type/mode by the retained opened object; prevent traversal/symlink/
replacement races. Sync and independently reread/validate the public/private
pair before marking SSH provisioned. Failure must not leave a partial key that
sshd trusts. Preserve a valid existing identity; corruption must disable SSH
with a diagnosis, not silently rotate it. Journal/storage copies mean wiping
RAM or unlinking does not establish physical secure erasure.

Existing installations use an explicit root provisioning command; missing
keys do not regenerate on every boot. Rotation is explicit and changes the
fingerprint. OS installation/boot can succeed with SSH disabled if provisioning
cannot complete, but must not claim SSH readiness. Live SSH stays disabled;
an optional explicit ephemeral mode later uses the same strong-random gate,
is clearly marked, and never borrows an installed machine's key.

**Gate / #52 closure:** two installations have different identities; reboot
and daemon restart retain one identity; OpenSSH parses the exported formats;
wrong permissions, truncation, pair mismatch, concurrent creation, ENOSPC,
OOM, short writes, failed barriers and interruption leave SSH disabled or the
old valid identity. Disposable installed-root BIOS/UEFI audits verify bytes,
metadata and unchanged templates/non-targets. No host secret in logs/artifacts.

### Phase 4 — SSH transport (#51 prerequisite)

Implement bounded identification/KEXINIT parsing, negotiated profile, transcript
hash, host signature, key derivation, NEWKEYS and authenticated packet records.
Handle split/coalesced TCP reads, short writes, EOF, maximum packet/padding
lengths and deadlines. A decrypted length remains untrusted until tag checking;
bound it before any allocation or dispatch. Support RFC-required packet sizes
within the proposed cap; reject excessive lengths deterministically.

Implement peer/local rekey, fresh ephemeral keys, per-direction sequence/key/
nonce state and hard byte/packet/time limits before reuse or wrap. Preserve
the first exchange hash as session identity. Implement negotiated strict KEX
with exact message ordering and sequence-reset rules from pinned OpenSSH
specifications; regression-test prefix truncation/unexpected pre-KEX messages.
Do not expose a knowingly vulnerable prototype as the production service.

**Gate:** independent OpenSSH handshake, fingerprint, packet protection and
forced rekey; wrong key/signature/tag/length/order rejected; deterministic
host transcript vectors plus sanitizer/fuzz coverage. Full session/SSH closure
does not follow from handshake success.

### Phase 5 — authentication and privilege separation

Keep private host keys and shadow data in a small privileged monitor. Run
unauthenticated parsing/transport work in a restricted spawned process; narrow
session-bound monitor operations must not become an arbitrary signing or
credential-change oracle. Close unrelated descriptors and validate all monitor
messages, lengths, session identity and operation ordering.
Provision a dedicated non-login service identity for preauth workers; dropping
capabilities while retaining a privileged UID or inherited secret fd is not
adequate separation. Do not assume an existing sandbox/chroot API.

Publickey verifies proof over the SSH session-bound authentication message,
not just key presence. Read bounded authorized_keys under safe ownership/path
rules. Initially accept plain Ed25519 entries; reject unsupported options or
certificates rather than silently discarding restrictions. No arbitrary client
environment, executable path, UID/GID or supplementary-group selection.

Password authentication reuses the authoritative account hash/generation
checks through a reviewed remote-auth path. Do not impersonate local terminal
login tokens. Unknown, locked, removed, empty-password and root accounts fail
generically. Apply bounded dummy verification where needed to limit account
enumeration, plus rate/admission limits before expensive verification. Wipe
passwords and avoid credential/plaintext logging.

Before child publication revalidate the account generation and effective
identity, set groups/GID/UID including saved IDs, drop all capabilities and
privileged descriptors, choose the validated home/shell and a fixed safe
environment. Prove the child cannot regain monitor authority. Use the existing
credential snapshot/publication protocol; auth success must not authorize a
stale or different account after concurrent updates.

**Gate:** good/bad keys/passwords, session-signature replay, username change,
account lock/delete/update races, rate limits, credential-drop failure and
descriptor exhaustion. Verify real/effective/saved IDs, groups/caps, shell
and helper exec inheritance, and zero cross-session privilege leakage.

### Phase 6 — PTYs, job control and session channels

Implement or establish an independently owned PTY pair per remote session,
separate from the framebuffer/UART terminal and all other SSH clients. Own
foreground process groups, echo/canonical/raw input, terminal size and signal
delivery. Map PTY requests, shell requests, window changes, EOF, exit status
and channel close with exact ownership and lifetime rules. Handle Ctrl-C,
Ctrl-Z, fg/bg and full-screen applications; disconnect must tear down only the
owned session and reap its children.

Prove duplex network/PTY progress under backpressure. A blocking socket read
cannot prevent draining child output or processing disconnect/rekey/window
updates. Choose bounded readiness support or explicit I/O processes with
retained fd ownership; a pipe-only command bridge is an intermediate test,
not an interactive terminal. Tie SSH window credits to real buffer capacity.

**Gate:** two isolated remote terminals plus local console, independent input
and foreground groups, resize, nano, long output, stopped/background children,
EOF/HUP/kill/disconnect and complete reference/VM/PTY reclamation.

### Phase 7 — daemon lifecycle and deployment

Add explicit configuration for enablement, bind address/port, password policy
and resource limits; reject unknown/unsafe settings. Bind port 22 through
existing authorization and the CPU-0 owner. No new protocol owner, NIC IRQ,
public affinity control or unbounded connection threads. Avoid crypto work
inside network/scheduler locks or in the CPU-0 protocol worker.

Use the selected bounded spawn/monitor model, monotonic admission/backoff and
finite cleanup. Boot only after installed-root, host-key and account readiness;
host-key/random failure disables the service with a clear reason. Start/stop/
restart and clean OS shutdown must close listeners, owned workers and children.

**Gate:** absent NIC, no DHCP, address changes, cable loss/recovery, stopped or
killed workers/monitor, half-open clients and capacity exhaustion while local
shell, DHCP, DNS and HTTP still progress. No leaked handles/credits, unauthorized
new sessions or indefinite waits. Existing installed data/media stay intact.

### Phase 8 — interoperability and physical acceptance

Record exact OpenSSH client versions, negotiation and isolated known_hosts
fixtures. Confirm normal supported clients connect without enabling obsolete
algorithms. Pin expected host fingerprints from trusted fixture public keys;
do not use blanket host-key-check bypasses as acceptance. Test deliberate key
rotation and the expected changed-key rejection separately.

Run primary BIOS/UEFI x SMP=1/4/8 installed-root cases with e1000e, plus
BIOS/UEFI e1000 smoke coverage, on disposable media only. Verify both auth
methods, two sessions, shell identity, job control, resize/nano, exact binary
data in both directions, rekey, malformed peers, timeout/cancellation,
reclamation, reboot fingerprint persistence and existing regression gates.
Keep captures encrypted; redact test secrets and forbid real user passwords
in retained test artifacts. Record packet-size/resource/CPU budgets separately
from throughput measurements.

Then prepare a fresh uninstrumented opt-in image and run a Dell installed
acceptance from Windows and Linux OpenSSH: verify fingerprint out of band,
authenticate, run shells, resize, interrupt/stop/resume, edit/save, reconnect,
test cable recovery and reboot. Verify ordinary AP syscall entry/CPU-0 owner
through a separate diagnostic build if the SSH-specific execution proof needs
it, without reopening completed Network SMP acceptance or forcing placement.

**#51 closes only after transport, both authentication methods, real isolated
interactive shells, deployment limits, automated gates and agreed physical
acceptance pass.** Report host, guest and physical evidence separately. This
milestone does not certify the implementation for hostile Internet deployment
or establish a general independent cryptographic audit.

## 5. Proposed implementation layout and tests

- `src/lib/crypto/` with a public freestanding header under `src/include/`;
  upstream code/license kept identifiable and shared by kernel/user builds.
- A separately reviewed random service and syscall adapter; no API number
  allocated in this plan and no reuse of weak TCP fallback material.
- `user/sshd/` for codec, transport, auth, monitor and session code;
  `user/ssh-keygen/` for bounded host-key provisioning and fingerprint output.
- PTY/session interfaces only after their ownership/locking design passes.
- Proposed targets: `test-crypto-host`, `test-random-host`, `test-ssh-hostkeys-host`,
  `test-ssh-transport-host`, `test-ssh-auth-host`, `test-pty-host`, then installed
  `test-ssh` and a separate physical handoff. Names are proposals, not existing
  runnable targets. No production boot defaults change in the planning commit.

Each milestone retains dependency/source/build identities, exact test counts,
argv, failure artifacts and bounded process cleanup. Tests use disposable files,
strict storage preflight, unchanged-source/non-target hashes and independent
filesystem audits where persistence is involved. Do not relabel failed attempts.

## 6. Primary specifications

- [SSH architecture, RFC 4251](https://www.rfc-editor.org/rfc/rfc4251),
  [transport, RFC 4253](https://www.rfc-editor.org/rfc/rfc4253),
  [authentication, RFC 4252](https://www.rfc-editor.org/rfc/rfc4252), and
  [connection protocol, RFC 4254](https://www.rfc-editor.org/rfc/rfc4254).
- [Curve25519/X25519, RFC 7748](https://www.rfc-editor.org/rfc/rfc7748),
  [Ed25519, RFC 8032](https://www.rfc-editor.org/rfc/rfc8032.txt),
  [ChaCha20-Poly1305, RFC 8439](https://datatracker.ietf.org/doc/html/rfc8439),
  [HMAC vectors, RFC 4231](https://www.rfc-editor.org/rfc/rfc4231).
- [SSH curve KEX, RFC 8731](https://www.rfc-editor.org/rfc/rfc8731.txt),
  [SSH Ed25519, RFC 8709](https://www.rfc-editor.org/rfc/rfc8709), and
  [KEX recommendations, RFC 9142](https://www.rfc-editor.org/rfc/rfc9142).
- [OpenSSH specifications](https://www.openssh.org/specs.html),
  [protocol extensions](https://raw.githubusercontent.com/openssh/openssh-portable/master/PROTOCOL),
  [private-key format](https://raw.githubusercontent.com/openssh/openssh-portable/master/PROTOCOL.key),
  [SSH ChaCha construction draft](https://datatracker.ietf.org/doc/draft-ietf-sshm-chacha20-poly1305/),
  [strict KEX draft](https://datatracker.ietf.org/doc/draft-miller-sshm-strict-kex/).

Check current errata/advisories and pin exact specification/upstream revisions
when implementation begins; moving upstream links are discovery references.
