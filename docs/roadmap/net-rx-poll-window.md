# Receive polling window — 2026-10-04

After the TX fast-poll change, the single stdout 1 MiB capture (client port
49154) spans 1.449551 seconds SYN through final ACK, with 1,048,782 server
payload bytes and no observed sequence gaps or overlaps. Median data-to-ACK
delay is 9.984 ms. The 142 window reopening pairs now appear essentially
together at the server capture; batching prevents measuring the replacement
delay precisely. Original capture retained as build/tcp-dell-fast-tx-retest.pcapng.
The approximately 10 ms cadence is consistent with timer fallback, not direct
proof of the worker's sleep state or an independently measured LAN RTT.

Approved next change increases the existing active RX window from 200 us to
1 ms. It starts after RX or published application work; empty turns never
renew it. Every active turn still yields cooperatively and executes bounded
RX draining plus protocol/deadline sweeps. Backward/stalled clocks retain the
4096-turn backstop and permanent worker clock fallback; unavailable clock and
cold-link handling are unchanged. Continuous progress can renew the window,
so this bounds grace after progress, not total CPU use during a busy transfer.
Preemption can extend wall time. No global quantum, IRQ, DMA, socket deadline,
buffer-size, TX wait or storage durability changes.

Host ASan/UBSan worker and TCP socket regressions PASS. Actual worker fake-clock
test delivers a response around 600 us, beyond the old budget, recycles exactly
once and returns to timer sleep. Exact 1 ms expiration, empty-turn nonrenewal,
backward, stalled and unavailable clock tests PASS. Strict build/image PASS.
QEMU TCG checks the timer fallback; it does not verify active TSC performance.
Dell throughput and active-load fairness remain physical retest gates.
Focused BIOS/UEFI e1000e TCP client wire tests PASS 2/2: independent Linux
peer, 64 KiB each direction, ABI/reset/caught signals/unread close and capture
audit. Both report zero idle worker CPU ticks over 500 ticks at 100 Hz.

Image: bin/fortress-ext4-test.img, local timestamp 2026-10-04 08:31:34.
SHA-256: 2b96cba2df2bbda5c343d5bfb885bca83bb929d73f7d19ae1deaa978f9f2f847.

After the TCP 120-second quiet period, capture one stdout download:

```sh
wget -q -O - http://192.168.0.153:8000/data-1m.bin | wc -c
```

Expected count 1048576. Record elapsed time and send the capture. Then verify
file download/hash/sync, shell responsiveness during a background transfer,
and return to idle. No subsecond throughput guarantee is made.
