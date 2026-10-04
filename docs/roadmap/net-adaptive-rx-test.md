# Adaptive RX grace experiment — 2026-10-04

Diagnostic Dell 16 MiB capture: 16.5044 s, 16,777,423 server bytes (207 header
bytes plus body), no observed sequence gaps/overlaps. Median data-to-ACK 8.675
ms; 1510/2358 data records above 8 ms. Photos show expiration delta 1692,
no clock failures/backstop, three yields above 1 ms; maximum yield 134.6 ms.
Wait deltas include inter-command idle and cannot all be assigned to TCP.
Capture retained at build/tcp-dell-16m-counters.pcapng.

User-approved experiment: normal grace stays 1 ms. Two RX batches less than
20 ms apart while a receive-capable TCP connection exists select 2 ms grace.
Established and local FIN_WAIT_1/2 states qualify, excluding orphan connections.
Quiet >=20 ms, non-TCP operation or cold link resets the streak. This is a
global worker heuristic; unrelated RX while TCP is active can contribute.
Empty polls never restart a deadline. Profile selection occurs on progress,
so expiry of an already started 2 ms window is unchanged by later quiet state.
No unbounded grace, timer reconfiguration, buffer change or busy idle loop.
Cooperative yield and full protocol sweeps remain each turn; clock fault
fallback and 4096-turn backstop remain. Sustained progress may renew grace.

Host worker/socket and strict build PASS. Tests cover 1->2 ms selection,
quiet/non-TCP reset, exact 2 ms expiry and actual-worker extended grace followed
by timer sleep. Physical performance/fairness and benefit remain unverified.
Final-image BIOS/UEFI USB smoke PASS 2/2, dmesg counter fields, writes/sync,
clean shutdown and Linux bytes/fsck. Evidence build/ext4-usb/run-eazdta1i.

Image bin/fortress-ext4-test.img: 2026-10-04 09:15:03 local.
SHA-256: 206648058a936d24a987609f3621beb919328f4325bd7d88cd13c51c5934518c.
The current working tree also contains user-tool changes made outside this
network experiment; they were preserved and the image builds that tree.

After the 120-second TCP quiet period, record counters, capture only wget,
then record counters again:

```sh
dmesg | tail -n 13
wget -q -O - http://192.168.0.153:8000/data-16m.bin | wc -c
dmesg | tail -n 13
```

Expected count 16777216. Compare with 16.50 s and 1692 expirations; send
snapshots, pcap and elapsed time. Check shell responsiveness with a background
transfer as a separate gate before accepting the longer grace.
