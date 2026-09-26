# Shell S7 Phase 1: anonymous pipes and syscall ABI

Implemented and verified on 2026-09-26. Architecture: [S7 plan](../plans/S7_PLAN.md).

`SYS_PIPE` (24) accepts a writable `int[2]` and flags of zero or
`VFS_O_CLOEXEC`. The complete output range must lie in canonical lower-half
user memory and pass writable page validation. Fewer than two available
descriptors returns `SYSCALL_EMFILE` before allocation. Descriptor publication
uses the existing single-threaded per-process fd-table ownership contract;
all allocation and installation failures unwind both endpoints.

Each pipe has 16 contiguous PMM frames accessed through the HHDM, a heap control
structure, and two anonymous stream nodes. Rank-2 locking serializes ring state
and reader closure. Writes up to 4096 bytes commit entirely under that lock or
return EAGAIN without modifying the ring; larger writes may be short. Atomic
data/space mirrors are release-published under the lock. No allocation, freeing,
scheduling or wakeup occurs while holding the pipe lock.

The VFS close callback runs on final `file_t` reference release, so duplicated
descriptors retain endpoint lifetime. The last writer closure allows buffered
data to drain before EOF; the last reader closure makes writes return
`SYSCALL_EPIPE` (-20). The final endpoint release frees both nodes, the control
structure and all 16 frames. Existing node types explicitly have no close hook.

Phase 1 uses `VFS_EAGAIN` (11), mapped to `SYSCALL_EAGAIN` (-21), for transfers
that would block. This additional ABI error preserves the distinction between
temporary emptiness and EOF. Scheduler blocking/wakeup, spawn CLOEXEC ordering,
shell pipelines and stream utilities remain later-phase work. The requested
close hook was implemented in Phase 1 despite its Phase 2 placement in the plan.

## Verification

Commands executed in WSL Ubuntu-24.04 at `/mnt/c/Sources/FortressOS`:

| Command | Result and evidence boundary |
| --- | --- |
| `make test-pipe-host` | PASS with ASan/UBSan and leak detection: real pipe/VFS/sys_pipe code; wraparound byte comparisons, partial transfers, 4095/4096/4097-byte atomic boundaries, EOF/EPIPE, dup reference lifetime, user validation rejection, descriptor exhaustion, all five heap allocation failures, PMM failure and both descriptor installation failures. Host memory, fd and page-validation adapters; single-threaded lock shim. |
| `make` | PASS: strict freestanding kernel build, ISO and raw image generation; image GPT/FAT/ext2 checks pass. |
| `make test-shell-host test-ext2` | PASS: shell/input/console host suites and eight ext2 block/sector/inode geometry combinations under sanitizers. |
| `make test-pipe` | PASS: BIOS and UEFI, 1 CPU, real Ring 3 syscall dispatch, writable page crossing, read-only/unmapped/kernel/noncanonical output rejection, flags, fd exhaustion without output modification, CLOEXEC, duplicate writer lifetime, EOF and EPIPE. Logs: `build/pipe-bios.log`, `build/pipe-uefi.log`. |

The QEMU runner replaces `/bin/shell` only in a temporary test ISO. It attaches
no data disks, uses read-only OVMF code and disposable vars, bounds execution and
terminates QEMU on all outcomes. The production initramfs is unchanged. Host
resource counters and sanitizers establish rollback/leak coverage; the QEMU
probe does not audit the PMM bitmap or establish concurrent SMP/physical-hardware
acceptance. `make test-host` groups pipe, shell and ext2 host suites.
