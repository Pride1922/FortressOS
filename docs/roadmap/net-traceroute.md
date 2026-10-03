# Finite ICMP traceroute — implementation and evidence

2026-10-03. `/bin/traceroute` implements the approved [64-byte probe ABI](../plans/NETCTL_TRACE_ABI.md), command 4 under existing SYS_NETCTL=42. Dell acceptance remains pending.

## Approved behavior

Each probe has a distinct command-local sequence label (1–90); output prints one line per probe without that label. Its timeout is bounded by the remaining 120-second whole-command budget. Reaching that deadline during a probe publishes PROBE_TIMEOUT, cancels the probe and terminates the command. EAGAIN reports ping/trace contention and exits 1 without retrying.

`user/traceroute.c` provides numeric IPv4 CLI, bounded hop/probe/timeout arguments and finite output. `src/include/trace_abi.h` asserts layout and offsets. `src/kernel/syscall.c` copies and validates the request, uses the existing wait channel, and publishes a completed copy only after revalidating the output range.

`src/net/net_ping.c` shares ping's finite mailbox, admission lock, token, wait channel and worker servicing. `src/net/net_ipv4.c` shares the existing pending slot and sends trace requests with the requested TTL. `src/net/icmp.c` parses bounded quoted headers. Correlation checks quoted tuple, IPv4 ID, echo identifier and sequence; destination echo replies also check the full payload token. Trace IPv4 ID 1 distinguishes it from unchanged ping ID 0. This is correlation, not authentication; stale traffic across reboot is not guaranteed distinguishable.

No scheduler, signal, lock-rank, wait-signature, timer-hook, DMA or driver change was made. The [wait lifecycle proof](../plans/TRACE_WAIT_LIFECYCLE_PROOF.md) states ownership and cancellation limits.

## Executed gates

- `make -j4`: freestanding build and normal raw-image GPT/FAT/ext2 verification PASS.
- `make test-net-trace-host`: actual kernel codec/IPv4/mailbox and CLI adapters under ASan/UBSan PASS. Covers 100/1000 Hz, whole-command cutoff, near-UINT64_MAX arithmetic, wire identity exhaustion without wrap, malformed/stale quotes, unreachable codes, cancellation/lease/link failure, no EAGAIN retry and unchanged ping. Includes 20,000 hostile inputs.
- `python3 scripts/test_net_trace.py --jobs 2`: BIOS/UEFI × e1000/e1000e socket backend, 4/4 PASS. Controlled two-router/destination path, forged quotes, unreachable, loss, ABI pointer/reserved/deadline fixture, contention, default Ctrl-C status 130, finite mailbox recovery and follow-up ping. Independent audit checks TTL progression, unique identities and incoming raw bytes against the injection log; missing, empty, malformed or incomplete inputs and application/capture disagreement fail the gate.
- Existing host ICMP, IPv4, ping, Ethernet worker, UDP/socket, TCP/socket, DNS and runtime configuration suites PASS.
- Existing TCP matrix 10/10 (`build/net2-step5/974c1ca9`), DNS matrix 10/10 (`build/net2-step7/1990da5f`), UDP matrix 10/10, ICMP matrix 8/8, link recovery 8/8 (`build/net-link/89d32c36`) and BIOS/UEFI ifup 10/10 each PASS.

Trace evidence is retained under `build/net-trace/`: final run `b05ea249`, earlier passing runs `032f860f` and `6e163591`. Each case retains disposable ISO/firmware, exact argv, UART, stderr, pcap, injection log, scenario expectations and audit results. Initial `4a4dd4d1` includes a retained contention-fixture prompt race; the runner now observes admission before issuing the competing trace. No data disks are attached.

Two existing fixture assumptions were corrected: Ethernet host adapters lacked NET-3 setters, and the link runner expected an obsolete nc error string. The latter now proves the listener remains blocked before carrier loss, exits 1 afterward and recovers with a fresh listener. No production nc or link behavior changed.

Host adapters prove logic rather than real IRQ exclusion. QEMU verifies the existing live BSP wait path and default SIGINT; it does not establish physical router compatibility, real latency, Ring 3 AP sockets or new live caught-signal/STOP behavior. DNS's loopback port-53 fixture ran as root with process-scoped Git safe-directory configuration.

## Physical gate

On the Dell, first trace a numeric LAN peer and compare the destination echo exchange in an independent Wireshark capture. Then use a controlled routed topology to verify successive TTLs and Time Exceeded quotations, an unreachable destination and a silent hop. Check contention with ping, Ctrl-C, budget termination and shell/ping recovery. Retain guest output, capture, topology and expected hop addresses. Ordinary Internet routers may suppress ICMP; stars alone do not establish a stack defect.
