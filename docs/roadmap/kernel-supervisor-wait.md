# Shell supervisor blocking wait — 2026-10-04

User explicitly approved extending the TCP fix beyond the network path after
the boot-calibrated TSC candidate still took seven seconds on Dell. Earlier
network hints, yields and polling candidates did not remove the repeated
20 ms client delays. See [capture evidence](net-tcp-poll-hints.md).

## Concrete problem and change

kmain supervised the shell with a process_is_alive loop and sti; hlt. The
main TCB remained RUNNING/READY rather than BLOCKED. HLT is a CPU instruction,
not scheduler sleep; selecting main could consume its ordinary two-tick
quantum. A cooperative network/reader yield therefore did not guarantee prompt
service of its intended peer. This code defect is established independently
of whether it explains every interval in the physical capture.

kmain now calls process_wait_quiescent(pid). This BSP kernel-only routine
snapshots a persistent exit sequence before checking liveness outside the
scheduler lock, then uses existing sched_wait_until on the child wait channel.
Its predicate is an atomic sequence comparison, with no nested scheduler or
process lock. It rechecks the target after every wake and reaps outside locks.
The existing exit-status collection and shell restart remain in kmain.

thread_exit publishes the sequence after marking the TCB TERMINATED under its
owner scheduler lock. This covers exits without userspace child records.
Publishing after terminal state prevents observing an exit event while target
liveness still requires a later unannounced transition. An exit between the
liveness check and blocked insertion changes the predicate; an earlier wake
cannot erase the event. BSP timer service bridges this sequence to the existing
child channel for AP exits. No scheduler lock is acquired inside the predicate
and no wake occurs under another scheduler lock. Existing BSP process-exit
wake behavior is retained. The 64-bit sequence traps rather than wrapping.

Global quantum, runqueue selection, context switch assembly, address-space
ownership, initialization ordering, network/NIC/USB settings and DMA quarantine
are unchanged. Main is no longer a runnable waiter during an ordinary live
shell session. User-only process_wait_child was not reused by the kernel.

## Verification

Strict build/image construction PASS. New make test-supervisor-wait PASS 4/4:
BIOS/UEFI at SMP=1 and SMP=4. Read-only GDB inspection verifies main is actually
on the blocked list with a wait channel, its CPU tick counter is unchanged
over a one-second idle interval, and three successive shell exit/restarts
return the supervisor to BLOCKED. These are live kernel checks, not host mocks.
UART/QEMU stderr evidence: build/supervisor-{bios,uefi}-{1,4}.log and
corresponding .stderr.log. Disposable ISO and paired OVMF copies; no data disks.
Final QEMU argv is checked and an added data drive is rejected.

Focused TCP client BIOS/UEFI e1000e PASS 2/2: real Ring 3, independent Linux
peer/pcap audit, 64 KiB both ways, ABI/reset/caught signals/unread close.
Each idle observation reports zero worker CPU ticks over 500 elapsed ticks.
Final optional EXT4 image smoke PASS BIOS/UEFI: writes, SYS_SYNC, mutation after
sync, clean poweroff and Linux bytes/fsck audit. Evidence retained at
build/ext4-usb/run-ki__25xk. make test-wget PASS: host sanitizers and eight
cases each under BIOS and UEFI, including stdout piping, downloaded bytes,
redirect/error handling and clean shutdown.

Delivered candidate image timestamp 2026-10-04 07:39:14, SHA-256
468baa25535cf0327773929bd0eec87498d6be8eca5fa9bdab052af78d4a03a3,
data PARTUUID 6807222C-32DB-433F-B260-C5490C738045. This fixes the runnable
supervisor wait; it is not yet evidence of improved Dell download speed.

SMP=4 proves BSP supervisor behavior while APs exist, not a tested AP target
exit race. The lost-wake guarantee above follows the event-publication order
and existing atomic sleep predicate; no claim of exhaustive interleaving tests.
Physical throughput and disappearance of the 20 ms stalls remain pending.
