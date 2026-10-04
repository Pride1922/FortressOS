# TCP Receive Buffer Expansion (64 KiB) — 2026-10-04

## Background & Architecture

Following the 32 KiB expansion, the TCP receive buffer was expanded to 64 KiB (`TCP_RXBUF_MAX 65536U`) to maximize unscaled TCP throughput across fast LANs without introducing the complexity of RFC 7323 TCP Window Scaling options.

### The 16-Bit Window Truncation Hazard

In an unscaled 16-bit TCP header, the receive window field is 16 bits wide, giving a maximum representable value of $2^{16} - 1 = 65535$.
If `TCP_RXBUF_MAX` is set to $65536$ ($2^{16}$), a naive window computation:
```c
(uint16_t)(TCP_RXBUF_MAX - c->rx_count)
```
evaluates to `(uint16_t)(65536 - 0) = (uint16_t)65536 = 0` when the receive buffer is completely empty. Advertising `Win=0` stalls the connection immediately.

Furthermore, if `c->rx_count` is stored as a `uint16_t`, receiving 65536 bytes wraps `rx_count` to 0, corrupting buffer fullness tracking.

### Solution

1. **Dual Constants in `src/net/tcp_tcb.h`**:
   ```c
   #define TCP_RXBUF_MAX   65536U   /* buffer size: power of two for fast modulo */
   #define TCP_WINDOW_MAX  65535U   /* maximum advertised window (16-bit field) */
   #define TCP_TXBUF_MAX   8192U    /* TX remains 8 KiB */
   ```
2. **Window Computation in `src/net/tcp_tcb.c`**:
   ```c
   static uint16_t window(const tcp_conn_t *c) {
       uint32_t free = TCP_RXBUF_MAX - c->rx_count;
       if (free > TCP_WINDOW_MAX) free = TCP_WINDOW_MAX;
       return c->eof ? 0 : (uint16_t)free;
   }
   ```
3. **`tcp_conn_t` Type Expansion**:
   `rx_count` is expanded to `uint32_t` so that $65536$ bytes in the buffer does not wrap to 0.
4. **Token Event Packing in `src/net/net_tcp.c`**:
   The 64-bit event token packs `rx_count` in bits 0..15 and `tx_count` in bits 16..31. To prevent bit 16 of `rx_count = 65536` from bleeding into `tx_count`, it is folded:
   ```c
   uint64_t rx_token = (c->rx_count & 0xffffU) ^ (c->rx_count >> 16);
   ```

## Verification & Test Results

1. **Unit Tests (`tests/net_tcp_tcb_host.c`)**:
   - `window_capping_and_buffer_limits()`:
     - `sizeof(a.rx) == 65536`
     - `sizeof(a.rx_valid) == 8192`
     - Empty buffer (`rx_count = 0`) advertises `window = 65535` (capped, no 0 truncation).
     - 1 byte occupied (`rx_count = 1`) advertises `window = 65535`.
     - 2 bytes occupied (`rx_count = 2`) advertises `window = 65534`.
     - Full buffer (`rx_count = 65536`) advertises `window = 0`.
   - `windows_and_reno()` updated to fill 64 KiB buffer before asserting zero-window persist behavior.
   - ASan/UBSan clean.
2. **Host Suites**:
   - `make test-net-tcp-tcb-host`: PASS (CB=82712, pool=662488).
   - `make test-net-tcp-host`: PASS.
   - `make test-net-tcp-socket-host`: PASS.
   - `make test-net-eth-host`: PASS.
   - `make test-net-socket-host`: PASS.
3. **Integration Suites**:
   - `make test-wget`: PASS (12/12 cases on BIOS and UEFI).
   - `make test-net-tcp`: PASS (10/10 matrix across BIOS/UEFI, e1000/e1000e, user/socket backends, and SMP 1/4 configurations).

## Physical Dell Latitude 5590 Verification (2026-10-04)

- **Command**:
  ```sh
  fortress:/ $ wget -O - http://192.168.0.153:8000/data-16m.bin | wc -c
  Connecting to 192.168.0.153:8000...
  connected.
  HTTP request sent, awaiting response... 200 OK
  Length: 16777216 bytes
  '-' saved [16777216/16777216] in 0.83s (19.27 MB/s)
  16777216
  fortress:/ $ 
  ```
- **Packet Capture**:
  Initial SYN / ACK packets advertise `Win=65535`.
- **Actual Measured Transfer**:
  - **Transfer size**: 16,777,216 bytes exact
  - **Elapsed time**: **0.83 s** (down from 1.45 s with 32 KiB, 3.5 s with 8 KiB, and ~16.5 s prior to scheduler optimizations)
  - **Throughput**: **19.27 MB/s** (~154.16 Mbps)
  - **Outcome**: The 64 KiB window completely removed the transport window bottleneck at ~5.6 ms RTT, cutting download time almost in half compared to 32 KiB!

