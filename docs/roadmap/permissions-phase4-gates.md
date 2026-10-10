# Permissions Phase 4: set-ID spawn and sudo

Phase 5 continuation from `700ba89` reruns and extends sudo 10/10, retains
secure-descriptor/USB nosuid controls and prepares a fresh normal boot image.
See [Phase 5 evidence](permissions-phase5-gates.md). The unchanged raw-image
hash and Phase-5-not-started statements below describe the Phase 4 checkpoint.

2026-10-10. Phase 4 complete locally. Local continuation from `81973c0`. No GitHub changes or Dell testing.
The two pre-existing untracked Phase 2 logs are preserved, with copies in
`build/permissions-phase4/baseline`. No reset, clean or commit is performed.

## Delivered behavior

The owned executable descriptor supplies the image identity; no pathname
relookup decides set-ID. Immutable TarFS bytes and metadata remain backed by
the reserved boot module. Runfs rechecks EXEC and snapshots bytes plus complete
metadata under its rank-1 lock into a private, bounded copy. Subsequent writes,
chmod, chown or removal cannot change the loaded snapshot. Unknown mutable
adapters without a snapshot and without nosuid fail closed with EOPNOTSUPP.
EXT2/EXT4 mounts always report nosuid/nodev; their existing ordinary executable
read behavior remains, without privilege transitions or an executable-write
freeze claim. No disk metadata or journal write path changes.

Setuid sets effective/saved UID to the admitted owner. A changed effective UID
gets CAP_ALL only when its new value is zero, otherwise zero capabilities.
Applying a root-owned setuid image to an already effective-root caller cannot
restore dropped capabilities. Group-executable setgid sets effective/saved GID;
non-group-executable setgid is ignored. Real IDs, supplementary groups and umask
remain inherited. nosuid ignores both bits and preserves ordinary inheritance.
Any admitted set-ID image receives secure handling, even when IDs already match.

All explicit cwd and OPEN file actions use caller authority. Ordered actions can
use inherited sources, including CLOEXEC sources. After actions, secure children
close descriptors above 2 unless their final destination was explicitly mapped;
the normal CLOEXEC sweep follows. Parent descriptors survive child construction
and failures. Child credentials are final before binding, staged release or
runnable publication. No registry/filesystem lock nesting is introduced.

The vector entry stack adds the first auxv pair AT_SECURE=23 with value 0 or 1,
then AT_NULL. The RDI/RSI/RDX register contract, entry alignment and 512-byte stack
floor remain. Shell and child builtin entry ignore imported environment on secure
entry. Other tools do not use environment for command lookup; sudo rebuilds the
target environment itself and only preserves bounded printable TERM.

`/bin/sudo` is root:root 04755 in normalized USTAR, independent of host modes.
It resolves real UID against the immutable database and requires wheel membership
for non-root callers. Authentication uses the user's SHA-256-crypt hash through
the controlling terminal, with no echo or cache. Locked/restricted accounts,
missing terminal, overflow, interrupted input and failed authentication reject
before credential transitions or target spawn. Password storage is wiped.
The approved passwordless operator default remains and sudo prints its own warning.
Root login remains locked; HOME remains temporary `/run/user/1000` at login.

After authentication sudo sets all GIDs to zero, installs root's groups and sets
all UIDs to zero. The target receives PATH=/bin, HOME=/root, USER/LOGNAME=root,
SHELL=/bin/shell and TERM. Bare commands resolve only under /bin; other commands
require an absolute path. Arguments are forwarded and child exit status returned.
Shell builtins can be invoked explicitly through `/bin/sh-builtin`.

## Verification

Executed in WSL Ubuntu-24.04; all final commands return zero:

```
make test-host
make test-perm-spawn-host test-perm-db-host test-perm-creds-host \
     test-perm-syscalls-host test-perm-runfs-host test-perm-privileges-host \
     test-perm-matrix-host
python3 scripts/test_runner_host.py
python3 scripts/audit_permissions_wiring.py
make test-sudo
make test-login
make -j4 bin/fortress.elf bin/initramfs.tar bin/fortress.iso
```

Final host logs: `build/permissions-phase4/final-aggregate-host.log`,
`final-focused-host.log`, `runner-host.log`. Actual policy tests cover 32,768
set-ID/nosuid/identity/capability combinations plus full-width IDs, final fd
destinations, empty/maximal entry stacks, alignment/floor and AT_SECURE. Actual
sudo syscall adapters cover wheel/locked/passwordless denial/admission, no-echo,
overflow draining/interrupted wiping, credential failures, minimal environment
and exit status. Runfs verifies the retained image/metadata after subsequent
mutation and zero publication on denied snapshots. Existing matrix/credential,
ABI/privilege, parser/crypt, login, shell/pipe/EXT2/stream/disk regressions pass.
The independent DAC oracle retains 1,376,256 access, 786,432 creation, 7,168
umask/bounds and 131,072 chmod cases. Source inventory: 173 classified sites,
zero unclassified paths. No new disk mutation or crash campaign is claimed.

Final sudo invocation: `make test-sudo`, `build/permissions-phase4/final-qemu.log`.
Retained campaign: `build/permissions-phase4/sudo-zg1qx04z`, manifest.json,
UART/stderr/argv/ISO/initramfs/USB images, Linux source fsck and snapshot hashes.
10/10 PASS:

| Cases | Assertions |
| --- | --- |
| BIOS/UEFI x SMP=1/4 password login (4) | Wrong password fails/no echo; correct sudo id/shadow access; minimal target environment; target status 1 propagated; plain shadow EACCES; setuid/SGID/AT_SECURE; unmapped fd 31 closed; explicit fd 9 mapping; privileged OPEN action denied with caller credentials; staged child keeps original umask/credentials; USB 04755 stays unprivileged; secure shell ignores hostile PATH/HOME; parent operator preserved |
| BIOS/UEFI SMP=1 non-wheel (2) | Authenticated operator without wheel is denied before sudo password/transition/target spawn; ordinary identity/shadow denial preserved |
| BIOS/UEFI SMP=1 passwordless (2) | Login and sudo warnings; approved passwordless elevation; same spawn/descriptor/environment/nosuid controls |
| BIOS/UEFI SMP=1 test-only root bypass (2) | Capset(0) followed by an admitted root-owned setuid image keeps caps=0; secure entry remains marked |

The QEMU backend is writable only to its disposable file. Guest boot policy is
RO, and every complete GPT image is SHA-256-identical after the case. Attempt 3
with QEMU readonly=on stalled UEFI before kernel UART; removing that backend
option allowed the RO guest campaign to run. This is fixture evidence, not a
physical controller workaround or a diagnosed firmware root cause. Preflight
rejects extra -drive/-blockdev/-snapshot storage arguments. Every QEMU PID is
terminated/reaped. Generated password hash files are removed after campaigns;
no known test password/hash is committed.

Login regression: `build/permissions-phase4/login-regression.log`, retained
`build/permissions-phase3/login-19cnpcui`, 9/9 PASS. Normal kernels ignore login=0
and contain no test credential syscall/escape. Production build log:
`build/permissions-phase4/production-build.log`; normalized production USTAR
confirms sudo root:root 04755, locked root and empty operator hash.
`preservation.json` verifies 441 final guest runtime/build/test input paths
against current sources, both original logs byte-for-byte and HEAD 81973c0.
Production ELF/initramfs/ISO hashes are retained. The normal raw disk image was
not rebuilt or attached; SHA-256 remains
`d130bec6d0b396a968135fb2132b9543a8774c8889b24fce464f88a10cd68c28`.

Earlier attempts remain retained: attempt 1 expected a nonexistent detailed
cat diagnostic, attempt 2 invoked missing /bin/env instead of sh-builtin env,
attempt 3 stalled UEFI with the QEMU RO backend, and attempt 4 incorrectly
expected a nonempty hash in the passwordless fixture. They are not final
acceptance evidence. Changes remain local and uncommitted; git diff --check
passes. Phase 5 is not started.

Host sanitizer coverage uses actual value helpers, the verbatim stack builder,
actual sudo helpers with syscall adapters, and actual VFS/runfs. It does not
certify IRQ or scheduling behavior. QEMU uses isolated source/build snapshots,
paired OVMF code/disposable vars, bounded UART waits, exact argv preflight and
PID termination/reap. Only disposable GPT/USB fixture files are attached; Linux
fsck audits the ext2 source and complete image hashes check guest RO behavior.
No physical-device, journaling, concurrent ordinary disk executable-write,
shared-user-thread or general cross-core socket claim is made.

Phase 5 hardening and physical acceptance remain separate work.
