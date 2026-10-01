# NET-2 step 1 — TCP design baseline and pure codec

2026-10-01. User authorized starting after engineering review. The seven-step
[plan](../plans/NET2_PLAN.md) and intended [socket contract](../plans/TCP_SOCKET_ABI.md)
are saved. ABI is not frozen or implemented; blocking/lifecycle proof and the
complete transport memory budget remain step 2/3 prerequisites.

Implemented tcp.h/tcp.c: byte-wise header encoding/decoding, IPv4 pseudo-header
checksum (mandatory even when the wire checksum is zero), bounded offset and
option parsing, SYN MSS encoding, safe parsing of window scale without offering
it, and skipping bounded unknown options. Malformed/duplicate MSS or scaling
options reject; EOL terminates option interpretation. Header fields use host
order, IPv4 addresses use network order. Payload view borrows the input buffer.
Input size is the exact IPv4 payload, not Ethernet padded length. Decoder never
publishes outputs on failure. No protocol state/flag negotiation is claimed.

Encoder supports overlapping payload storage, snapshots header values before
writes and rejects unavailable scaling output. SYN MSS output is optional;
local-MTU/peer-MSS segmentation belongs to the future transport, not this codec.
The pure codec allows IPv4 transport lengths up to 65515 bytes; the eventual
live path still obeys interface MTU and rejects fragmented IP packets.

Actual freestanding stack frames: pseudo-header checksum helper 64 bytes,
decode 80 bytes, encode 96 bytes. New codec compiles with -Os,
-Wframe-larger-than=512 and -fstack-usage. No stack packet array, allocation,
lock, scheduler/NIC interaction or retained DMA pointer. The stack figures
are individual frames, not a bound on all transitive kernel/IRQ execution.

Verification executed:

- make test-net-tcp-host: ASan/UBSan PASS. Independent checksum and fixed SYN,
  zero/odd/max length, corruption/wrong pseudo-header, zero-valued valid checksum,
  offset/option bounds, truncated headers, unaligned input, in-place encode,
  unknown/EOL/NOP/MSS/scale options, overflow/null arguments, unchanged failure
  outputs and 20000 deterministic random-input iterations (also checksum-sealed).
- make bin/fortress.iso: strict freestanding build and ISO packaging PASS.
- make test-net-udp-host test-net-socket-host test-net-host: PASS (Phase 0 103/103).
- make test-net-eth-host test-net-ipv4-host test-net-icmp-host
  test-net-ping-host: ASan/UBSan PASS.
- make test-net-pci: BIOS/UEFI NIC present/absent 4/4 PASS. This is an
  existing boot/discovery regression, not TCP traffic acceptance.
- git diff --check: PASS.

TCP remains disconnected from net_input, worker, socket table and syscall
dispatch. No handshake/data transfer, sockets, TCP QEMU acceptance or physical
TCP claim follows. Existing UDP/ping, driver workarounds, SYS_NETCTL and all
protected scheduling/locking/entry contracts remain unchanged. Step 2 builds
the deterministic transport engine/simulator before live socket integration.
