# Permissions Phase 1 permissive wiring

2026-10-10. Phase 1 complete locally. No GitHub updates or enforcement. Phase 0
ownership, metadata, ABI and lifetime work remains in place. See
[protocol](../plans/PERMISSIONS_PROTOCOLS.md) for required Phase 2 decisions.

## Current call and ownership map

| Operation | Actor source and admission | Lifetime / exclusion |
| --- | --- | --- |
| OPEN, STAT, STAT_EXT, MKDIR, UNLINK, RENAME, CHDIR | Actual current published snapshot; actor-aware VFS entries | G released before owned walk; existing filesystem references and mutation callbacks retained |
| READDIR | Actual snapshot and READ hook on descriptor node | Existing descriptor/node ownership; no new pathname lookup |
| Spawn image, explicit cwd, FD actions, inheritance | One actual actor snapshot, EXEC image/cwd and action access masks | Executable open pin; owned cwd lookup released before construction; child receives complete same value |
| Single/group/signal-0 user signals | Current actor and eligible target credential bindings under G | Hook immediately before pending publication; no pointers escape, wakes after unlock |
| REBOOT | Actual snapshot and permissive CAP_SYS_BOOT | Before freeze/sync and power entry; invalid command has no side effects |
| BIND low port | Actual snapshot and permissive CAP_NET_BIND | Before network locks; network-to-host port conversion; zero/high ports exempt |
| Boot/mount/kernel probes, initial terminal FD | Explicit trusted `_kernel` entry | Existing initialization and owned reference contracts |
| READ/WRITE/dup/close and pipe/socket FDs | Previously admitted handles | Existing descriptor/open-pin lifetime; no new pathname permission decision |
| Terminal signals, CHLD, writer SIGPIPE | Trusted kernel publication | Existing registry publication and unlocked wake protocol |
| Fixed /mnt metrics query | Trusted fixed lookup | No user pathname or mutation |

Sources: `src/fs/vfs.c` lookup_common/open_common and actor wrappers;
`src/kernel/syscall.c` syscall_actor and filesystem dispatch;
`src/kernel/thread.c` process_spawn_from_vfs_group/process_spawn_internal;
`src/kernel/process_table.c` signal_send_common;
`src/net/net_socket_syscall.c` SYS_BIND. The public VFS header documents actor
and trusted APIs. `src/include/permissions.h` contains permissive value hooks.
Filesystem callbacks are indirect owned lookup/create/open/metadata/mutation
operations, not additional user entry points. They do not yet authorize under
their authoritative exclusion. Signal and capability hooks are also permissive.

Creation now carries actual euid/egid and raw requested mode end to end. OPEN
defaults to 0644; spawn action mode is retained. Applying umask and parent
setgid rules belongs to Phase 2. No new syscall number or stat layout is added.
Explicit spawn cwd is normalized against the caller's cwd and admitted before
FD actions; missing/non-directory cwd fails. Executable metadata must be regular.

## Verification and evidence

- `make test-host`: existing aggregate host regression. Host adapters do not
  prove IRQ/scheduler correctness.
- `make test-perm-wiring-host`: actual VFS with a non-root actor, zero caps,
  complete value comparisons, walk/access/delete masks, explicit creation
  mode/IDs, owned lifetime cleanup, trusted bypass and missing actor rejection.
- `make test-perm-signal-wiring-host`: actual registry publication, current
  target values, staged/group/signal-0, credential replacement and detachment;
  pthread lock adapter checks G ownership and wakes outside G.
- `make test-perm-capability-host`: actual BIND/UDP manager with low-port
  byte-order boundaries 0,1,53,256,1023,1024,4096 and non-root zero-cap actor;
  actual reboot syscall/registry snapshot with mocked power/flush, invalid
  command, flush failure, both commands and missing published binding.
- `make test-perm-phase1 PERM_PHASE1_FIXTURE=<explicit Phase0 journaled fixture>`:
  fresh isolated source/build, trace kernel, BIOS/UEFI shell acceptance and
  BIOS/UEFI USB Ring 3 × SMP=1/4. Source inventory currently classifies 139
  sites with no unclassified production path/trusted calls. This finite
  inventory does not prove arbitrary control-flow coverage.
- Normal untraced kernel runs the same four USB guest cases separately.

Evidence is retained under `build/permissions-phase1`: host-final.log,
manifest.json, audit.json, trace-build.log, trace-shell.log, raw shell logs,
trace-guest.log and normal-guest.log. Manifest identifies the isolated source
workspace containing its source snapshot and raw guest logs/trace records.
No Ring 3 AP execution or physical result is inferred from SMP=4 boot.

Final results: aggregate and three focused host targets PASS; trace shell
2/2 PASS; trace USB guests 4/4 PASS; normal USB control 4/4 PASS. Final trace
source workspace is `guest-workspace-un9kbeif`, with raw guest evidence
`build/permissions-phase0/guest-jncwwcj_`; normal guest evidence is
`build/permissions-phase0/guest-fl7ttzif`. All three final commands exited 0.
Final diff whitespace check passes; no QEMU process remains from these runs.

The initial trace-shell attempt exposed an observation race in the harness:
it resumed after finding the blocked reader, then stopped again to inspect it.
The harness now retains the stop that admitted that observation. State,
descriptor/reference and timer checks remain. Trace records are removed only
from application-output matching when explicitly requested; raw UART evidence
is retained. Earlier failed output matching is in
`failed-trace-shell-output.log`. Host test construction errors and initial
compile diagnostics were corrected before final runs; they are not passes.

## Enforcement gate remains closed

Do not enable DAC by changing the permissive stub. Authoritative filesystem
adapters must revalidate identity and decide from protected values under their
own exclusion, without nesting G. Canonical `.`/`..` path traversal and
same-path rename no-op/error precedence require explicit semantics and tests.
Descriptor rights, umask/setgid and denied-mutation byte/allocation/journal
oracles remain Phase 2 work. Phase 5's whole-syscall policy audit is separate.
No login, sudo, devfs/runfs expansion, cross-core sockets, allocator tuning,
TLB change or Dell testing was performed for this phase.

Normal journaled EXT4 `bin/fortress.img` remains unchanged:
`d130bec6d0b396a968135fb2132b9543a8774c8889b24fce464f88a10cd68c28`.
ELF/initramfs/ISO are rebuilt; acceptance uses disposable images. EXT2 shell
fixtures remain explicitly named EXT2 tests, not normal image creation.
