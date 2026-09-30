# Networking Phase 0 — Core Abstractions, Buffers, and Header Parsers

**Status**: COMPLETE (2026-09-30)  
**Deliverables**: `src/include/net.h`, `src/net/checksum.[ch]`, `src/net/eth.[ch]`, `src/net/arp.[ch]`, `src/net/ipv4.[ch]`, `tests/net_host.c`, `Makefile` (`test-net-host`).  
**Verification**: Host test suite with AddressSanitizer and UndefinedBehaviorSanitizer: 103/103 tests passing (100% PASS). Freestanding kernel build clean (`make` passes).

---

## 1. Overview & Scope

Phase 0 implements the pure-software foundation of the FortressOS networking subsystem (Milestone NET-1). In accordance with `docs/plans/NET_PLAN.md` and project invariants (`AGENTS.md` §4, `PROTECTED.md`):
- **Zero hardware, zero driver, zero DMA, zero interrupts, zero kernel modifications**: No kernel schedulers, memory managers, or device trees are modified in Phase 0.
- **Freestanding C11 implementation**: All parser and codec logic in `src/net/` and `src/include/net.h` is freestanding C11 (using project-native types and string primitives).
- **Pure functions**: All parsers take explicit `(const void *buf, size_t len, ...)` parameters. There are zero mutable global variables and zero dynamic allocations.
- **Strict bounds validation**: Every protocol field read is strictly guarded against input buffer length `len`. Malformed, truncated, runt, or oversized buffers are rejected with negative error codes before any out-of-bounds memory can be accessed.

---

## 2. Implemented Components & Abstractions

### 2.1 Device Interface (`net_dev_t`) & Packet Buffer (`pbuf_t`) — `src/include/net.h`

- **`net_dev_t`**: Mirrors `block_dev_t` (`src/drivers/block.h`). Provides device abstraction for Ethernet interfaces:
  ```c
  typedef struct net_dev {
      char      name[16];         /* e.g., "eth0" */
      uint8_t   mac_addr[6];      /* Interface MAC address */
      uint32_t  mtu;               /* Maximum transmission unit (typically 1500) */
      uint32_t  flags;             /* NET_UP, NET_RUNNING */
      int     (*send_packet)(struct net_dev *dev, const void *buf, size_t len);
      int     (*poll_rx)(struct net_dev *dev);
      void     *priv;
  } net_dev_t;
  ```
- **`pbuf_t`**: Implements the exact layout specified in `NET_PLAN.md` §3.1:
  - Intrusive singly-linked list pointer (`next`).
  - Mutable `payload` pointer within `data` storage, enabling protocol layers to advance headers without copying.
  - `len` (current segment length) and `total_len` (entire packet length).
  - Explicit lifecycle flags (`PBUF_FLAG_ALLOCATED`, `PBUF_FLAG_RX`, `PBUF_FLAG_TX`).
  - Flat `data[PBUF_CAPACITY]` storage (`PBUF_CAPACITY = 2048` bytes).
- **Endianness primitives**: `htons`, `ntohs`, `htonl`, `ntohl` implemented with type-safe bit shifts, host-independent and freestanding.

### 2.2 RFC 1071 Checksum Library — `src/net/checksum.[ch]`

- Implements standard 16-bit ones' complement Internet checksum computation.
- **`net_checksum(const void *data, size_t len, uint32_t initial)`**:
  - Handles odd-length buffers by padding the final byte with zero in the lower octet (RFC 1071 §4.1).
  - Operates correctly on unaligned memory buffers.
  - Host-endian independent: sums 16-bit words in network byte order.
- **Incremental accumulation**: Provides `net_checksum_accumulate` and `net_checksum_finish` to support multi-buffer checksums (e.g. UDP/TCP pseudo-headers spanning separate memory regions).

### 2.3 Ethernet II Frame Codec — `src/net/eth.[ch]`

- **`eth_header_t`**: 14-byte standard Ethernet II header (`dst_mac[6]`, `src_mac[6]`, `ethertype`).
- **EtherTypes**: `ETHERTYPE_IPV4 = 0x0800`, `ETHERTYPE_ARP = 0x0806`.
- **Validation**:
  - Minimum frame validation (`ETH_MIN_FRAME_LEN = 60` bytes excluding FCS; payloads below 46 bytes require padding or are noted as runt if total frame < 14 bytes).
  - Maximum frame validation (`ETH_MAX_FRAME_LEN = 1514` bytes).
  - Decode rejects buffers smaller than 14 bytes (`-1`) or exceeding 1514 bytes (`-2`).
- **MAC Filtering**:
  - `eth_is_broadcast(mac)`: Returns true if MAC is `FF:FF:FF:FF:FF:FF`.
  - `eth_mac_equal(a, b)`: Returns true if two MAC addresses match.
  - `eth_mac_matches(mac, host_mac)`: Promiscuous-ready address filter accepting matching unicast or broadcast frames.

### 2.4 RFC 826 ARP Codec & Cache Stub — `src/net/arp.[ch]`

- **`arp_packet_t`**: 28-byte ARP packet layout for Ethernet/IPv4:
  - Hardware type (`0x0001` Ethernet), Protocol type (`0x0800` IPv4).
  - Hardware address length (6), Protocol address length (4).
  - Opcode (`0x0001` Request, `0x0002` Reply).
  - Sender MAC & IP, Target MAC & IP.
- **Codecs**: `arp_encode_request`, `arp_encode_reply`, `arp_decode`.
- **Validation**: Rejects buffers `< 28` bytes, null pointers, unsupported hardware/protocol types, or non-matching address length fields.
- **ARP Cache Stub (`arp_cache_t`)**:
  - Bounded 16-entry table (`ARP_CACHE_CAPACITY = 16`).
  - State machine entries: `FREE`, `RESOLVING`, `RESOLVED`.
  - Timestamp-based LRU eviction when capacity is reached.
  - Pure functions: `arp_cache_init`, `arp_cache_lookup`, `arp_cache_update`.

### 2.5 RFC 791 IPv4 Header Codec — `src/net/ipv4.[ch]`

- **`ipv4_header_t`**: 20-byte base IPv4 header with packed bitfield accessors:
  - Version and IHL (Internet Header Length) validation.
  - Total length field bounds checking against buffer size.
  - Identification, TTL, Protocol (`IPV4_PROTO_ICMP = 0x01`, `IPV4_PROTO_UDP = 0x11`).
  - Source and destination IPv4 addresses.
- **Checksum Verification**:
  - `ipv4_calculate_checksum`: Computes the ones' complement header checksum with field zeroed.
  - `ipv4_decode`: Verifies header checksum; rejects corrupted headers with `-5`.
- **Fragment Rejection**:
  - Inspects `flags_frag` for `More Fragments (MF = 0x2000)` and non-zero `Fragment Offset (0x1FFF)`.
  - Enforces NET-1 specification: fragment reassembly is out of scope; any fragmented packet is cleanly rejected with `-6`.
- **Length & Bounds Validation**:
  - Validates Version == 4 (`-2`), IHL >= 5 (`-3`), total length <= buffer length (`-4`).

---

## 3. Host Test Evidence (`make test-net-host`)

The host test fixture `tests/net_host.c` exercises all abstractions under GCC with `-O2 -fsanitize=address,undefined -Wall -Wextra -Werror`:

```
========================================================
FortressOS Networking Phase 0 — Host Test Suite
========================================================
[1] Testing RFC 1071 Checksum Implementation...
  [PASS] RFC 1071 standard vector matches
  [PASS] Checksum over buffer with odd length matches
  [PASS] Checksum of all-zeros is 0xFFFF
  [PASS] Checksum of all-ones (0xFF) is 0x0000
  [PASS] Multi-buffer accumulation matches contiguous buffer
  [PASS] Unaligned buffer checksum matches aligned buffer
[2] Testing Ethernet II Frame Codec & MAC Filtering...
  [PASS] Broadcast MAC recognized
  [PASS] Unicast MAC not recognized as broadcast
  [PASS] MAC equality helper verified
  [PASS] MAC matching (unicast + broadcast) verified
  [PASS] Ethernet encode succeeds
  [PASS] Ethernet decode validates header, payload, and EtherType
  [PASS] Runt frames (< 14B) and oversized frames (> 1514B) rejected
[3] Testing RFC 826 ARP Codec & 16-Entry Cache...
  [PASS] ARP request encoding produces valid 28-byte packet
  [PASS] ARP request decoding validates sender/target fields
  [PASS] ARP reply encoding and decoding round-trip verified
  [PASS] Truncated (< 28B) and malformed ARP packets rejected
  [PASS] ARP cache lookup on empty cache returns -1
  [PASS] ARP cache update and resolution verified
  [PASS] ARP cache update existing entry verified
  [PASS] ARP cache 16-entry capacity and LRU eviction verified
[4] Testing RFC 791 IPv4 Codec, Checksum, & Fragments...
  [PASS] IPv4 encode and decode round-trip verified
  [PASS] Valid IPv4 header checksum accepted
  [PASS] Corrupted IPv4 header checksum rejected with -5
  [PASS] More Fragments (MF) packet rejected with -6
  [PASS] Non-zero fragment offset packet rejected with -6
  [PASS] Malformed version (< 4) rejected
  [PASS] Malformed IHL (< 5) rejected
  [PASS] Truncated header (< 20B) rejected
  [PASS] Payload length exceeding buffer rejected
[5] Testing Packet Buffer (pbuf_t) Lifecycle...
  [PASS] pbuf_init sets fields and capacities correctly
  [PASS] pbuf_header advances payload and updates lengths correctly
  [PASS] pbuf_header releases header space on negative offset
  [PASS] pbuf_header bounds check prevents underflow and overflow
  [PASS] pbuf linked-list chaining verified
[6] Testing Resilient Bounds & Fuzz-style Robustness...
  [PASS] 50 truncated buffers rejected with zero ASan faults
  [PASS] Corrupted buffer bytes rejected with zero ASan faults

All 103 tests passed successfully! [100% PASS]
```

### 3.1 Kernel Build Verification

Executing `make` in the repository confirms that adding `src/net/` and `src/include/net.h` to the freestanding build succeeds with 0 errors and 0 warnings:
```
[OK] Bootable ISO generated: bin/fortress.iso
[OK] Bootable Raw Disk Image generated: bin/fortress.img
[VERIFY] Checking raw disk image: bin/fortress.img
  [PASS] Protective MBR verified (Type 0xEE, valid boot signature)
  [PASS] Primary GPT Header and Partition Array CRC verified
  [PASS] Partition 1: ESP (FAT32, 64 MiB) containing BOOTX64.EFI verified
  [PASS] Partition 2: Linux FS (ext2, 64 MiB) verified with 0 errors via e2fsck
  [PASS] Backup GPT Header and Partition Array verified
```

---

## 4. Verification Boundaries & Next Steps

Phase 0 establishes the stateless, host-testable core. Later phases will build upon this foundation:
- **Phase 1**: e1000 PCI discovery, BAR MMIO mapping, descriptor ring allocations, loopback transmit.
- **Phase 2a**: Polling-based receive ring processing and frame reception.
- **Phase 2b**: Dell Latitude physical bring-up.
- **Phase 3**: ARP resolution engine and ICMP echo responder.
- **Phase 4**: UDP socket layer and network configuration (`net=<ip>/<prefix>,<gateway>`).
