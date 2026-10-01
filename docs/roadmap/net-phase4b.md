# NET Phase 4b — Dell physical IPv4/ICMP acceptance (2026-10-01)

Status: **COMPLETE — physical LAN-peer gate accepted (2026-10-01).** This file
records **manual, user-supplied hardware evidence**, not an automated test pass.
Phase 4a's host/QEMU evidence remains the automated logic gate and is preserved
separately in [net-phase4a.md](net-phase4a.md).

## Configuration and method

- Machine: Dell Latitude 5590, integrated Intel I219-LM (`8086:15D7` at
  `0000:00:1F.6`, MAC `C8:F7:50:0E:35:80`), booted from USB.
- Guest boot config: `net=192.168.0.168/24,192.168.0.1`.
- Guest commands via the real Ring 3 shell `/bin/ping`:
  `/bin/ping -c 4 192.168.0.1` and `/bin/ping -c 4 192.168.0.222`.
- Second host: a Windows 11 machine at `192.168.0.222` on the same wired LAN,
  used both as a ping peer and as a Wireshark capture host.

## Observations (user-supplied)

### Guest → gateway (`192.168.0.1`)

- `/bin/ping -c 4 192.168.0.1`: **4/4 replies, 0% loss**, reported **20 ms**
  RTT, and a usable shell afterward.

### Guest → LAN peer (`192.168.0.222`)

- Ping to the Windows 11 peer **succeeded** after inbound ICMP was allowed
  through the peer's firewall.

### Second-host Wireshark (screenshot)

- Four matched ICMP Echo Request/Reply pairs, sequences **1–4**, all frames
  **74 bytes**: frames **923/924, 931/932, 942/943, 947/948**.

### Reverse direction (Windows 11 → FortressOS)

- `ping 192.168.0.168` from the Windows 11 peer: **4/4 replies, 0% loss,
  TTL 64**; RTT **min 3 ms, max 17 ms, average 9 ms**.

## Evidence boundaries

- This is **manual, user-supplied hardware evidence**, not an automated pass.
- The Wireshark **screenshot** confirms four matched outbound request/reply
  pairs; the **reverse-direction** success is evidenced by the Windows
  **terminal output**, not by a capture.
- **No raw pcap was supplied**, so this record does **not** claim independent
  payload/checksum verification or any saved capture artifact. (Phase 2b
  retained a cable-side pcap; Phase 4b did not.)
- The guest-reported RTT **includes polling and scheduling effects** — ingress
  is polling-only on the BSP worker with the tick-bounded wake — so it is not a
  wire-latency or best-case measurement.
- No sustained-load acceptance, measured idle-CPU percentage, or cross-core
  protocol delivery is claimed; the BSP worker remains the sole protocol owner.
- The addresses, gateway and on-link peer describe one LAN on one machine.
  Record the exact config per run; do not hardcode it elsewhere.

## Next

Phase 5 (UDP/sockets) is next. Automated IPv4/ICMP and Ring 3 ping evidence:
[net-phase4a.md](net-phase4a.md) and the annex
[§4 test targets](../subsystems/net.md#4-test-targets-and-verification-notes).
