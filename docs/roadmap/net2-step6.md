# NET-2 Step 6 — Dell 5590 physical TCP acceptance

2026-10-02. COMPLETE by explicit user acceptance. NET-2 is 6/7 done;
Step 7 (userspace DNS + nslookup) remains.

The user's [physical session notes](evidence/net2-step6-session-notes.md)
are retained verbatim as the session record. Dell Latitude 5590, I219-LM
8086:15D7, MAC c8:f7:50:0e:35:80, guest 192.168.0.168/24, gateway 192.168.0.1.
The Windows peer was 192.168.0.153 over Wi-Fi; Wireshark ran on that adapter.

## Accepted physical cases

| Case | User-confirmed application and wire result |
| --- | --- |
| A: guest client / echo peer | Exact echoed request, clean teardown, MSS 1460 both sides, no RST/retransmission; prompt and ping 1/1. |
| B: guest client / Python HTTP server | HTTP/1.0 200 OK, Content-Length 21 and expected body; prompt and ping 1/1. |
| C: peer client / guest nc listener | 21-byte inbound payload printed after tty-skip update; adjacent data/FIN acknowledged, clean teardown, no RST/retransmission; prompt and ping 1/1. |
| D: peer abort | RST produces an nc error without hanging; usable shell and ping 1/1. |

These are manual physical observations accepted by the user, not a new automated
test target. Raw physical pcaps were not supplied for a local independent audit;
the wire observations and acceptance are attributed to the user's session.
Acceptance applies to this Dell 5590 setup, not untested models or general TCP
conformance. Cable-before-boot/link behavior is recorded in the session notes.

## Userspace correction and automated evidence

The original nc listener consumed terminal stdin before receiving. At the user's
request, listener mode now queries fd 0 using existing TERM_ISATTY and skips the
stdin copy only for a terminal. It still accepts one child, closes the listener,
calls SHUT_WR and drains receive to EOF. Pipes/files and client mode retain their
serial input behavior. Terminal-query errors use normal descriptor cleanup.

No transport, socket ABI, scheduler, signal, lock-rank, wait signature, timer,
DMA or driver behavior was changed. The data-before-EOF implementation was
already correct; [Case C investigation](net2-step6-caseC.md) records its proof.

The updated tool passes ASan/UBSan adapter tests and
`make test-net-nc-data-fin`: BIOS/UEFI x tty/null/file/pipe, **8/8 PASS** with
actual Ring 3 nc and independent complete-stream capture/injection audit
(build/net2-step6/19464db8). Sixteen host READ/RECV cases verify data+FIN before
or after accept, combined/adjacent arrival, full/partial reads and listener
closure. Strict make and generated raw-image filesystem checks pass.

The investigation's full reruns passed Step 3 5/5, final Step 4 invocation 5/5,
and Step 5 matrix 10/10; those executions predate the tty behavior update.
The post-update focused tests above are the agent-executed evidence for the
change. The session notes additionally report those suites passing after the
fix; that statement is retained as user-provided evidence. Earlier concurrent
Step 4 timeout attempts remain documented rather than counted as passes.
