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
The v1 request accepts no paths, keys, source pointers, block maps or caller
admission flags.

## Additive source ingestion (v2)

RDX=96 selects the exact `pkg_request_v2` layout: the first 64 bytes use the v1
field offsets with size=96, version=2, operation=7 (`PKG_INSTALL_SOURCE`) and
nonzero session/canonical package name. Four u64 fields follow: catalog file
descriptor, catalog length, artifact file descriptor, artifact length. Both
descriptors must be less than MAX_PROCESS_FDS. All reserved fields stay zero.
The output remains the 80-byte v1 result. Other size/version/operation combinations
refuse; no v1 reserved field is reinterpreted.

Catalog length is 1..262144; artifact length is 128..8405120 (existing envelope,
maximum manifest and maximum compressed payload). Request and writable output
ranges are validated before allocation/callbacks. An
admitted backend must explicitly implement source installation; otherwise the
operation returns EOPNOTSUPP. The current authorized session pins an operation
reference before a single nonblocking memory credit is acquired. Both opened
files are retained before any I/O. Only regular, nonstream, readable files with
the exact declared size are accepted. A private file position copies from zero
in bounded reads and checks exact EOF without changing either shared descriptor
offset. No user source-memory mapping is retained across allocation or I/O.
At most one
catalog/artifact snapshot is allocated, bounded by the sum of those limits.
The source snapshots are kernel-owned and freed on success and every failure;
retained file references are released on all paths as well.
Cancellation/current credentials are checked again after copying. No spinlock
is held across allocation, copies, verification or storage.

The callback must reverify those immutable bytes against configured repository public keys
and current installed-version policy, derive its own plan, and durably retain recovery
inputs before publishing transaction intent. Source pointers expire when the
callback returns. Supplying bytes or receiving a userspace verification result
does not authorize activation. A successful callback must report coherent IDLE;
error paths preserve userspace output and never retry the callback.

Normal frontend syntax is `pkg install PACKAGE ABSOLUTE-STAGING-DIRECTORY`.
It opens `catalog.v1` and `artifact.fpk` through NOFOLLOW regular-file
descriptors, checks bounded sizes, submits one v2 request and releases its session
even on read/installation/output errors. Existing name-only install/remove and
v1 callers retain their original behavior. Staging files remain available for
inspection. No policy or secret is read from staging.

This implements transport, not the production backend binding. The normal
kernel still has no replacement simple source-installation backend, and the executor
remains disabled by default. Host syscall/backend models prove copy isolation,
owner/session validation and cleanup, not a physical package installation.

A boot without a registered installation backend returns EOPNOTSUPP before
allocating an owner or invoking storage. Backend registration remains a trusted
kernel interface; downloaded source bytes cannot register a backend. Device
enrollment and boot-selector authority are not requirements of the replacement
package workflow.

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
cannot take trust options from argv. No device enrollment is required by the package workflow.

The enrolled-root normal constructor/backend and package-specific boot modes
have been removed. The existing transaction engine remains in source and its
fixture suites, dormant in normal builds until the simple installation backend
is connected. Download verification now uses ordinary repository public-key
configuration. This is not physical installation/upgrade acceptance.
