# NET-2 Step 6 Case C — data/FIN delivery investigation

2026-10-02. Physical failure reported on Dell 5590. The raw caseC-bug.pcapng
has not been supplied locally; packet observations below are user-reported.
The user subsequently confirmed plain `nc -l 7777` with terminal stdin and
accepted the test-invocation diagnosis. The physical retry subsequently passed;
Step 6 is closed by user acceptance in [the checkpoint](net2-step6.md). No
TCP/socket runtime ordering fix is claimed.

## Findings

The proposed EOF-before-buffer defect does not reproduce in current source.
tcp_conn_peek first selects rx_count bytes, and tests error/EOF only when that
count is zero (tcp_tcb.c). net_tcp_peek delegates to it, and net_tcp_syscall
copies positive bytes to the validated user buffer before consuming them.
Kernel SYS_READ dispatch sends stream sockets to the same TCP handler.
The frozen TCP_SOCKET_ABI already requires buffered bytes before FIN EOF.

The supplied zero-window observation does not establish an undrained RX queue:
window() returns zero whenever c->eof is true, independently of rx_count.
ACK 23 is consistent with accepting 21 bytes plus SYN and FIN sequence space.
Neither ACK/window alone proves which userspace descriptor is blocked.

The original nc_main explicitly ran copy(0,fd), then SHUT_WR, then copy(fd,1). Plain
`nc -l 7777` therefore read terminal stdin before receiving the socket data.
Remote FIN does not close local stdin. The original receive-only workaround was:

```
nc -l 7777 < /dev/null
```

## Terminal-aware listener update

At the user's request, nc -l now probes fd 0 with existing
SYS_TERMCTL(TERM_ISATTY, 0, 0). A terminal result skips only the stdin copy;
SHUT_WR and receive-until-EOF remain. Nonterminal stdin (pipe, file, /dev/null)
keeps the serial copy/EOF/SHUT_WR flow. Client mode always copies stdin. Probe
errors fail with ordinary cleanup. Single-accept behavior and all kernel/socket
APIs remain unchanged. Plain `nc -l 7777` is now the physical Case C command.

Host sanitizer cases verify terminal skip without consuming input, unchanged
binary nonterminal short writes, unchanged terminal-backed client copying and
terminal-query failure cleanup. The live data/FIN target now covers tty, null,
file and pipe stdin in BIOS and UEFI, with exact peer consumption and independent
complete-stream capture/injection audit. This update changes only userspace
tool behavior; it adds no polling, scheduler, signal or driver changes.

Executed update verification: `make test-net-nc-data-fin` **8/8 PASS**,
BIOS/UEFI x tty/null/file/pipe, saved under build/net2-step6/19464db8. The target
also passes sixteen host socket data/FIN cases and nc ASan/UBSan adapters.
`make` rebuilds ISO and bin/fortress.img with the updated nc; the raw image's
MBR/GPT/FAT32/ext2 checks pass, e2fsck reports zero errors. No physical USB was
written. Earlier full Step 3/4/5 results below predate the tty behavior update;
the new update is validated by these focused actual nc tests.

This preserves finite serial stdin/SHUT_WR/receive and single-accept semantics.
In the initial investigation only usage guidance and ABI documentation of the tool were clarified; there is
no socket ABI change. No scheduler, signal, lock-rank, wait, timer, DMA, driver,
handshake, retransmission or window logic was changed. If the physical command
had already redirected stdin, this explanation would have been insufficient: inspect the raw
capture and blocked descriptor before declaring a root cause. The reported
second-session handshake also needs the raw capture/run boundaries; it does
not independently prove accept or read delivery.

## Added tests

listener_data_fin_tests in tests/net_tcp_socket_host.c runs sixteen cases:
combined data+FIN or adjacent data then FIN without an intervening read/tick;
arrival before or after ACCEPT; READ or RECV; full or partial initial read.
The listener closes before receive. Every case receives exact ordered bytes,
then repeated EOF, with child data retained and no allocations leaked. PASS
under ASan/UBSan on unchanged production protocol/socket behavior.

scripts/test_net_nc_data_fin.py sends the literal 21-byte physical-test payload
and FIN together in one segment to actual Ring 3 nc with /dev/null stdin.
The guest prints the payload, exits, and runs a following shell command.
Complete-stream independent wire audit and injection matching PASS.
Saved evidence: build/net2-step6/ba725f23/nc-data-fin. BIOS/e1000/socket,
disposable ISO, no data disks; this is not physical acceptance.
The added `make test-net-nc-data-fin` target rebuilt the ISO with the receive-only
usage guidance and repeated the focused gate successfully (b223c9d7).

Executed: make test-net-tcp-socket-host test-net-nc-host; make
test-net-tcp-host test-net-tcp-tcb-host test-net-tcp-fixture; and
python3 scripts/test_net_nc_data_fin.py. Host and focused QEMU gates PASS.
Full Step 3: python3 scripts/test_net_tcp_client.py, 5/5 PASS, including SMP=4.
Final full Step 4: python3 scripts/test_net_tcp_server.py, 5/5 PASS including
SMP=4 after the Step 5 matrix finished. This is a separate full invocation.
Step 5: python3 scripts/test_net_tcp_matrix.py --all --jobs 2, 10/10 PASS
in one invocation (135e29fe), including both SMP=4 cases.
The first Step 4 invocation passed four SMP=1 cases but its SMP=4 first echo
timed out; original logs/capture are retained under
build/net2-step6/step4-smp4-first-attempt. A focused concurrent rerun also timed
out and is retained under step4-smp4-second-attempt. Do not label either failed
invocation a pass. The later full invocation passes; load sensitivity is an
unproven explanation, not a diagnosed scheduler/transport bug or a fix claim.
