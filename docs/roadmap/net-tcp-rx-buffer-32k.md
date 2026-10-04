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

## Dell Hardware Acceptance & Measurement (2026-10-04)

Bare-metal run on physical Dell Latitude 5590 (I219-LM NIC, 1 Gbps LAN link, booted via UEFI):

```sh
fortress:/ $ wget -O - http://192.168.0.153:8000/data-16m.bin | wc -c
Connecting to 192.168.0.153:8000...
connected.
HTTP request sent, awaiting response... 200 OK
Length: 16777216 bytes
'-' saved [16777216/16777216] in 1.45s (11.03 MB/s)
16777216
fortress:/ $ 
```

**Results:**
- **Transfer size**: 16,777,216 bytes exact (verified by `wc -c`)
- **Elapsed time**: **1.45 s** (Target was < 2.0 s; previous was ~3.5 s, and ~16.5 s prior to scheduler fixes)
- **Throughput**: **11.03 MB/s** (~88.24 Mbps)
- **Outcome**: Target achieved and surpassed.

## 32 KiB vs 64 KiB Evaluation

- **Result with 32 KiB**:
  32 KiB was completely sufficient to reach 11.03 MB/s and complete the 16 MiB stream in 1.45 s on physical Dell hardware, comfortably beating the < 2.0 s acceptance target.
- **64 KiB assessment**:
  Because 32 KiB already delivers 1.45 s transfers without queue bloat or additional memory overhead, 64 KiB is not immediately required. It remains an available future optimization if higher LAN throughput (approaching line rate) is desired, as 65535 still fits within the unscaled 16-bit TCP header window.
