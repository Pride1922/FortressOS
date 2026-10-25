# Normal package control and source integration

SYS_PKGCTL is allocated at the reviewed next slot, 95. Numbers 200/203 remain
private test adapters and are absent from the ordinary dispatcher. The public
request/result are defined in src/include/pkg_abi.h: exact 64/80-byte v1 value
records with fixed offset assertions. Existing syscalls and layouts are unchanged.

Input registers are RDI=request, RSI=writable result, RDX=64. Size is checked
before access. Both complete ranges are validated before copying/side effects.
Wrong version/size, unknown operation, nonzero reserved fields or invalid
session/package shape return EINVAL without writing output. Bad ranges return
EFAULT. Output is copied exactly once after capturing the result; no operation
is retried after losing output. Result reserved fields are zero.

Operations 1–4 are status/acquire/recover/release. Status/acquire require session
zero; recover/release require a nonzero session. Their inline package bytes
must all be zero. Operations 5/6 install/remove require a nonzero session and a
canonical 1–32-byte package identity, with zero padding after a shorter name.
No paths, keys, source pointers, block maps or caller admission flags are accepted.

An ordinary unenrolled boot has no registered backend and returns EOPNOTSUPP,
before allocating an owner or invoking storage. Backend publication is a
boot-only trusted kernel call after authenticated projected-root admission,
recovery, account preparation and root publication. Its context and callback
table are immutable for the mount lifetime; rebinding is refused. Compile-time
executor selection is not admission. Production enrollment/key delivery remain
separate from the existing QEMU TEST admission adapter.

The syscall binds the current TCB to the generation arena extracted from the
accepted owner implementation. Acquire rechecks effective UID zero and
CAP_SYS_ADMIN. Mutation/recovery retain operation references and check current
credentials between bounded engine steps. A pending signal or credential drop
prevents subsequent steps; an admitted step retains its durable outcome.
Status/recovery/mutation share the backend's nonblocking execution credit and
capture coherent results before releasing it. No spinlock is held across I/O.

Lifecycle trace: process_exit and thread_exit call pkg_owner_exit before
existing teardown; duplicate notification is harmless after clearing the key.
An operation reference must drain before disposal. fd_close_cloexec calls
pkg_owner_exec; sched_reap_dead checks the key after dropping scheduler
exclusion. Spawn creates a zeroed, unbound TCB. The key is TCB-local and no
session is inherited or transferred to another thread/process. This preserves
the accepted private lifetime protocol; it does not introduce shared pthread
session ownership or an in-place exec acceptance claim.

Normal /bin/pkg links the existing parser and the system frontend. Each
install/remove/recover command acquires lazily and releases after completion
or failure, including output failure. Status acquires no writer. Inspect
remains read-only; verify still requires a separately provisioned policy and
cannot take trust options from argv. Default enrollment remains absent.

The executor's filesystem hooks are under FORTRESS_PKG_ENGINE, default zero.
The isolated test adapter selects this code and supplies the previously
accepted TEST admission; hardware identities and fw_cfg secrets remain in
tests/. Before physical mutation, replace those fixture assumptions with the
reviewed hardware admission and enrollment procedure in the Fortress 1.0 plan.
