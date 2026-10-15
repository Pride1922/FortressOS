# DHCP Client v1 and Broadcast Probe — Implementation and Evidence

2026-10-15. `/sbin/dhcpc` (DHCP client) and `/bin/dhcp-probe` (broadcast probe) implement RFC 2131 dynamic IPv4 configuration and kernel broadcast RX/TX verification. Physical Dell 5590 acceptance remains pending.

## Approved Scope and Design

- **Kernel changes:** Minimal, strictly localized to IPv4 broadcast RX, routing, and remote address admission:
  - `src/net/net_ipv4.c`: `net_ipv4_input` accepts destination `0xFFFFFFFFu` (limited broadcast) alongside `s_cfg.local_ip`. Broadcast ICMP echo requests drop silently (preventing smurf amplification).
  - `src/net/net_ipv4.c`: `net_ipv4_route` accepts `dest == 0xFFFFFFFFu` and routes directly with `*hop = 0xFFFFFFFFu`.
  - `src/net/net_ipv4.c`: `net_ipv4_udp_start` uses source IP `0.0.0.0` when destination is broadcast (`0xFFFFFFFFu`).
  - `src/net/net_ipv4.c`: `net_ipv4_tick` bypasses ARP resolution when `next_hop == 0xFFFFFFFFu` and sets destination MAC directly to `FF:FF:FF:FF:FF:FF`.
  - `src/net/net.c`: `net_socket_address_snapshot` permits `0xFFFFFFFFu` as a valid remote address for `SYS_SENDTO`.
- **Shared Codec (`user/dhcp_codec.[ch]`):**
  - Implements standard RFC 2131 BOOTP/DHCP message layout (op, htype, hlen, hops, xid, secs, flags, ciaddr, yiaddr, siaddr, giaddr, chaddr, sname, file, magic cookie `0x63825363`, options).
  - Encoders for `DHCPDISCOVER` and `DHCPREQUEST` with Parameter Request List (Subnet Mask, Router, Domain Name Server).
  - Decoder `dhcp_decode_reply` validating magic cookie, transaction ID matching, and bounded option parsing.
- **Broadcast Probe (`/bin/dhcp-probe`):**
  - Binds UDP socket to `0.0.0.0:68` (requires `CAP_NET_BIND` via `sudo`).
  - Broadcasts `DHCPDISCOVER` to `255.255.255.255:67`.
  - Bounded busy-wait poll for up to 5 seconds via `SYS_SYSINFO` (`uptime_ticks` / `tick_hz`).
  - Prints responder IP and port on receipt (`DHCP probe: received response from <ip>:<port>`), exits 0 on receive, 1 on timeout.
- **DHCP Client (`/sbin/dhcpc`):**
  - Full DHCP 4-way handshake: DISCOVER -> OFFER -> REQUEST -> ACK.
  - Matches XID across all stages, parses offered IP (`yiaddr`), Subnet Mask (Option 1), Gateway/Router (Option 3), and DNS server (Option 6).
  - `--dry-run` (`-n`) flag performs the handshake and prints configuration without mutating interface state or writing files.
  - Applies runtime network configuration atomically via `SYS_NETCTL` (`NETCTL_IFSET`).
  - Writes resolver configuration to `/tmp/resolv.conf` (or target path) and interface config to `/tmp/network.conf` and `/etc/network/network.conf` (if root writable), followed by `SYS_SYNC`.
  - V1 limitation: one-shot client with bounded busy-wait; no `SYS_NANOSLEEP`, background daemon, or lease renewal. Lease persists until reboot or manual reconfiguration. V2 (with `SYS_NANOSLEEP` and renewal) is deferred to the time subsystem milestone.

## Executed Gates

- `make -j4`: Freestanding C build, ISO, and disk image generation PASS.
- `make test-dhcpc-host`: Host ASan/UBSan unit test covering DHCP packet encoding, decoding, magic cookie validation, XID matching, option parsing, malformed packets, and truncation. 100% PASS.
- `make test-net-ipv4-host`: Host ASan/UBSan unit tests validating broadcast RX acceptance, broadcast routing, and broadcast ICMP drop. 100% PASS.
- `make test-net-socket-host`: Host ASan/UBSan unit tests validating broadcast remote destination acceptance in `SYS_SENDTO`. 100% PASS.
- `make test-dhcp-probe`: BIOS and UEFI QEMU `-net user` test. Verifies probe binds port 68, sends broadcast DISCOVER, receives reply from QEMU DHCP server at `10.0.2.2:67`, and exits 0. 100% PASS.
- `make test-dhcpc`: BIOS and UEFI QEMU `-net user` test (9/9 cases each):
  1. `--help` / `-h` usage display.
  2. `--dry-run` handshake without mutating interface.
  3. Real lease acquisition (`10.0.2.15/24`, gateway `10.0.2.2`, DNS `10.0.2.3`).
  4. Interface verification via `/sbin/ifconfig`.
  5. Connectivity verification via `/bin/ping 10.0.2.2`.
  6. Resolver verification via `/bin/nslookup example.com`.
  7. Configuration persistence verification via written configuration files.
  8. Interface re-apply verification via `/sbin/ifup`.
  9. Clean exit status 0.
- All test runs executed with disposable ISO/OVMF fixtures, with evidence retained under `build/dhcp-probe-*` and `build/dhcp-client-*`.

## Physical Acceptance Protocol (Dell Latitude 5590)

To physically verify DHCP client on bare-metal hardware:
1. Boot FortressOS on Dell Latitude 5590 via UEFI or BIOS with wired Ethernet cable connected to a network with an active DHCP server.
2. Log in as `operator` and check existing network status:
   ```bash
   ifconfig
   ```
3. Run the broadcast probe to confirm physical broadcast exchange:
   ```bash
   sudo /bin/dhcp-probe
   ```
   Confirm output prints: `DHCP probe: received response from <server_ip>:67`.
4. Run the DHCP client in dry-run mode:
   ```bash
   sudo /sbin/dhcpc --dry-run
   ```
   Confirm offered IP, netmask, gateway, and DNS servers match local LAN settings.
5. Acquire and apply the lease:
   ```bash
   sudo /sbin/dhcpc
   ```
   Confirm lease is applied:
   - `ifconfig` displays assigned IP and gateway.
   - `ping -c 4 <gateway_ip>` returns 4/4 replies.
   - `nslookup <known_domain>` returns valid DNS records.
6. Verify persistence:
   - Inspect `/etc/network/network.conf` (or `/tmp/network.conf` in live mode).
   - On installed root, reboot the system and verify `/sbin/ifup` restores the configured settings.

Dell Latitude 5590 physical bare-metal acceptance user-confirmed PASS (2026-10-15): broadcast transmission, reception, dynamic lease acquisition, and interface configuration verified on real hardware.

## DHCP v2 Deferred Roadmap & Time Subsystem Dependency

The next version of the DHCP client (v2) requires the **Time Subsystem** and cannot be completed without it:
- **`SYS_NANOSLEEP` / Kernel Sleep Syscalls:** v1 relies on bounded busy-wait polling via `SYS_SYSINFO` tick counters. v2 requires true blocking sleep syscalls (`SYS_NANOSLEEP`) to suspend execution during receive windows and timer intervals without consuming CPU cycles.
- **Monotonic Wall-Clock & Real-Time Lease Accounting:** v2 needs monotonic time and real-time clock tracking to record lease duration, calculate exact expiration timestamps, and schedule state transitions.
- **Background Lease Renewal Daemon:**
  - RFC 2131 timers: $T_1$ (renewal timer, $0.5 \times \text{lease duration}$) and $T_2$ (rebind timer, $0.875 \times \text{lease duration}$).
  - Background daemon that sleeps until $T_1$ to send unicast `DHCPREQUEST` to the original leasing server.
  - If unacknowledged, broadcasts `DHCPREQUEST` at $T_2$ to rebind with any available DHCP server.
  - Graceful lease release on system shutdown via `DHCPRELEASE`.
- **Boot-Time Automatic Configuration Hook:**
  - Support `net=dhcp` on the Limine kernel command line or a boot service script that automatically triggers `dhcpc` before shell/login initialization.
