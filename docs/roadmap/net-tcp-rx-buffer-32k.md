# TCP Receive Buffer Expansion (8 KiB to 32 KiB) — 2026-10-04

## Background & Motivation

On LAN environments (e.g. Dell Latitude 5590 over 1 Gbps link with ~5.6 ms observed RTT), TCP throughput with FortressOS was capped by the maximum advertised receive window:
$$\text{Throughput}_{\max} = \frac{\text{Win}_{\max}}{\text{RTT}} = \frac{8192 \text{ bytes}}{0.0056 \text{ s}} \approx 1.46 \text{ MB/s}$$

This physical delay capped 16 MiB downloads to ~3.5–16 seconds regardless of scheduler responsiveness or NIC fast-polling.

## Implementation Details

The TCP buffer size constant was internal to the transport implementation:
1. `src/net/tcp_tcb.h`:
   - Split shared `TCP_BUFFER_SIZE 8192U` into independent transmit and receive limits:
     - `#define TCP_TXBUF_MAX 8192U` (TX remains bounded at 8 KiB)
     - `#define TCP_RXBUF_MAX 32768U` (RX buffer increased to 32 KiB)
   - Updated `tcp_conn_t`:
     - `uint8_t tx[TCP_TXBUF_MAX], rx[TCP_RXBUF_MAX];`
     - `uint8_t rx_valid[TCP_RXBUF_MAX/8];` (occupancy bitmap increased to 4096 bytes)
2. `src/net/tcp_tcb.c`:
   - Updated `window()` calculation: `return c->eof ? 0 : (uint16_t)(TCP_RXBUF_MAX - c->rx_count);`
   - Updated receive paths (`receive_data`, `tcp_conn_peek`, `tcp_conn_consume`) to use `TCP_RXBUF_MAX`.
   - Updated transmit paths (`tcp_conn_init`, `ack_new`, Reno fast recovery, `tcp_conn_queue`, `copy_tx`) to use `TCP_TXBUF_MAX`.
3. `src/net/net_tcp_syscall.c`:
   - Sized static staging buffer `received` to `TCP_RXBUF_MAX` (32768 bytes), allowing single-syscall drains up to userspace buffer capacity.
4. Window scale:
   - 32768 fits comfortably in the unscaled 16-bit TCP header window field (max 65535); no RFC 7323 window scale option is needed.

## Verification & Test Results

- **Strict Build**: Clean compilation without warnings (`-Wall -Wextra -Werror`, `-Wframe-larger-than=512 -fstack-usage`).
- **Unit & Host Regression**:
  - `make test-net-tcp-tcb-host`: PASS (ASan/UBSan; verified 32 KiB buffer zero-window persist, Reno retransmit, out-of-window RST, 2 MiB clean and lossy stress simulations).
  - `make test-net-tcp-host`: PASS (codec, options, wire serialization).
  - `make test-net-tcp-socket-host`: PASS (manager, syscalls, 64 KiB bi-directional stream).
  - `make test-net-eth-host`: PASS.
  - `make test-net-socket-host`: PASS.
- **QEMU Acceptance**:
  - `make test-wget`: PASS (All 12 cases across BIOS and UEFI boot).
  - `make test-net-tcp`: PASS (10/10 matrix across BIOS/UEFI, e1000/e1000e, user/socket backends, and SMP 1/4 configurations).

## Dell Hardware Instructions & Target

On the physical Dell Latitude 5590 after the 120-second TCP quiet time:

```sh
wget -O - http://192.168.0.153:8000/data-16m.bin | wc -c
```

Target:
- Advertised window in pcap: `Win=32768` (initial free space) opening up from 0 to 32768.
- 16 MiB stream elapsed time: target under 2.0 s (previously ~3.5 s).
- Byte count: 16777216.

## 32 KiB vs 64 KiB Evaluation

- At 5.6 ms RTT, 32 KiB raises the window-limited ceiling to $\approx 5.85 \text{ MB/s}$.
- If physical testing shows LAN throughput reaching ~5.8 MB/s (16 MiB in ~2.8 s), 32 KiB will have relieved the 8 KiB bottleneck. If under 2 s is strictly needed on a 5.6 ms RTT link, the window ceiling would need:
  $$\text{Window} \ge \frac{16 \text{ MiB}}{2.0 \text{ s}} \times 0.0056 \text{ s} \approx 47 \text{ KiB}$$
- 64 KiB (65535 max unscaled) could be considered as an immediate next step if 32 KiB does not break the 2-second threshold, without requiring TCP window scaling options.
