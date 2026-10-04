# NET-3 — Runtime Network Configuration & Resolver Integration

Status: COMPLETE (2026-10-02). Kernel ABI extension for `NETCTL_IFGET` and `NETCTL_IFSET`, userspace utilities `/bin/ifconfig` and `/bin/ifup`, shared `/mnt/.fortress/network.conf` parser, and DNS server integration into `/bin/nslookup` and `/bin/nc` implemented and verified under ASan/UBSan host suites and QEMU BIOS/UEFI matrix.

---

## 1. Scope & Architecture

NET-3 delivers runtime interface inspection and reconfiguration from Ring 3 without rebooting:
1. **`/bin/ifconfig`**: Read-only query displaying MAC, IPv4 address/CIDR, netmask, broadcast, default gateway, MTU, link state (`UP`, `WAITING (no cable)`, `FAILED`), and 64-bit monotonic packet counters.
2. **`/bin/ifup`**: Applies network configuration either from persistent config file (`/mnt/.fortress/network.conf`, or alternate specified file) or via CLI arguments (`<ip>/<prefix> [gateway]` or `<ip> <netmask> [gateway]`). Supports `--dry-run` and `--help`.
3. **Shared config parser (`user/netconf.h` / `user/netconf.c`)**: Bounded 4 KiB file, <= 256 B line length, `#` comments, `=` or whitespace separators, keys: `address` (CIDR), `gateway` (IPv4), `dns` (IPv4). Duplicate keys overwrite with warning; unknown keys ignored with warning; malformed values reject with nonzero exit.
4. **Static DNS integration (`/bin/nslookup` and `/bin/nc`)**: When `-s` is omitted, reads `dns` key from `/mnt/.fortress/network.conf`. If missing or no `dns` key, prints diagnostic and exits nonzero.

### Kernel ABI (`SYS_NETCTL = 42`)
- **`NETCTL_IFGET` (2u)**: Takes `netctl_ifget_t *` (64 bytes). Validates struct_version == 1, reserved == 0. Locklessly checks link state via `e1000_link_state_abi()` (mapping driver states to stable ABI values 0–4: `UNINITIALIZED=0`, `WAITING=1`, `ONLINE=2`, `DOWN=3`, `FAILED=4`). Samples 64-bit RX/TX packet counters. Reads IP configuration under Rank-1 `g_net_stack_lock`.
- **`NETCTL_IFSET` (3u)**: Takes `const netctl_ifset_t *` (32 bytes). Enforces:
  - Caller must be on BSP (`SYSCALL_EOPNOTSUPP` returned before user memory access).
  - `struct_version == 1`, `flags == 0`, `reserved == 0`, `reserved2 == {0, 0}`.
  - `local_ipv4` must be valid unicast host (not 0, not loopback, not >= 224, not 255.255.255.255).
  - `netmask_ipv4` must be contiguous 1s, prefix 1–30.
  - Subnet host bits of `local_ipv4` cannot be 0 (network) or all 1s (broadcast).
  - `gateway_ipv4` (if nonzero) must be valid unicast host on-link (`(gw & m) == (ip & m)`), not equal to `local_ipv4`, and not subnet network or broadcast.
  - Device must be present and link state must be `NET_IF_LINK_ONLINE`; any other state returns `SYSCALL_EIO`.
  - Atomically updates `s_config`, clears ARP cache via `arp_cache_init()`, and updates `net_ipv4`, `net_socket`, and `net_tcp` under Rank-1 `g_net_stack_lock`. ISN clock and salt are preserved per invariant 4b.

---

## 2. Verification Evidence

### Host Sanitizer Evidence (`make test-net-ifconfig-host`)
Compiled with `-fsanitize=address,undefined -Wall -Wextra -Werror`:
- `sizeof(netctl_ifget_t) == 64` and `sizeof(netctl_ifset_t) == 32` static assertion and runtime checks.
- Absent controller `SYSCALL_EIO` and null pointer validation.
- Monotonic 64-bit packet counter publication.
- Link state mapping across all 5 ABI values.
- `/bin/ifconfig` exact output formatting for `ONLINE`, `WAITING (no cable)`, and `FAILED` states.
- `net_validate_ifset` full failure matrix: bad version, non-zero flags/reserved, loopback/multicast/broadcast local IP, non-contiguous mask, prefix 0/31/32, subnet network/broadcast local IP, off-link gateway, gateway == local IP, and link state != ONLINE.
- `net_set_config` multi-layer state sync across IPv4, socket, and TCP layers.
- Config parser: valid CIDR, CRLF line endings, duplicate keys (last wins with warning), unknown keys (ignored with warning), malformed address rejection, file > 4096 bytes rejection, line > 256 bytes rejection.
- `/bin/ifup`: `--help`, `--dry-run`, CIDR CLI, dotted-decimal CLI, and config file parsing.

### QEMU Integration Evidence (`make test-net-ifconfig` & `make test-net-ifup`)
Tested under QEMU q35 with e1000 across **BIOS and UEFI** (SMP=1, disposable ISO/OVMF, strictly verified argv preflight with no data disks):
- **Case 1 (initial ifconfig 10.0.2.15/24)**: PASS on BIOS and UEFI.
- **Case 2 (ifup --help)**: PASS on BIOS and UEFI.
- **Case 3 (ifup --dry-run no-op)**: PASS on BIOS and UEFI.
- **Case 4 (ifup CIDR apply 10.0.2.50/24)**: PASS on BIOS and UEFI.
- **Case 5 (ifup dotted apply 10.0.2.60/24)**: PASS on BIOS and UEFI.
- **Case 6 (malformed address rejection)**: PASS on BIOS and UEFI.
- **Case 7 (ifup config file /etc/network.conf)**: PASS on BIOS and UEFI.
- **Case 8 (ping gateway after reconfiguration)**: PASS on BIOS and UEFI (re-applies `10.0.2.15/24` and pings SLIRP gateway `10.0.2.2`).
- **Case 9 (ifup missing /mnt/.fortress/network.conf diagnostic)**: PASS on BIOS and UEFI.
- **Case 10 (nslookup missing DNS server diagnostic)**: PASS on BIOS and UEFI.

### Regression Targets
- `make test-net-dns-host`: PASS.
- `make test-net-nc-host`: PASS.
- `make test-net-host`: 103/103 PASS.

---

## 3. Physical Acceptance (Dell Latitude 5590, 2026-10-02)

Verified on bare-metal Dell Latitude 5590 with integrated Intel I219-LM (`8086:15D7`):
1. **`/bin/ifconfig`**: Displays physical MAC (`c8:f7:50:0e:35:80`), IP/netmask, gateway, link UP, and monotonic packet counters.
2. **On-the-fly CLI reconfiguration**: `/bin/ifup 192.168.0.199/24 192.168.0.1` and dotted-decimal CLI form apply cleanly without reboot.
3. **Persistent file application**: Saved `/mnt/.fortress/network.conf` (`address 192.168.0.199/24`, `gateway 192.168.0.1`, `dns 1.1.1.1`) applied successfully via `/bin/ifup`.
4. **Connectivity**: Gateway ping succeeds after runtime reconfiguration.
5. **DNS integration**: `/bin/nslookup` resolves using the DNS server from `/mnt/.fortress/network.conf` when `-s` is omitted.
6. **User confirmation**: All physical test cases passed on hardware with clean prompt return and no kernel faults.

---

## 4. Positional DNS Arguments & Hostname Resolution (2026-10-04)

1. **Positional DNS in `/bin/ifup`**:
   - Accepts up to two optional DNS server addresses following the gateway in both CIDR and dotted-decimal notation:
     - `ifup <ip>/<prefix> <gateway> [<dns1>] [<dns2>]`
     - `ifup <ip> <netmask> <gateway> [<dns1>] [<dns2>]`
   - Automatically writes `nameserver <ip>\n` entries to `/tmp/resolv.conf` (and `/mnt/.fortress/resolv.conf` when `/mnt` is writable).
2. **Userspace Stub Resolver Multi-Server Fallback (`user/dns.c`)**:
   - When `-s` is omitted, loads nameservers sequentially from `/tmp/resolv.conf` or `/mnt/.fortress/resolv.conf`.
   - Queries configured servers in priority order, falling back to secondary DNS servers upon timeout (`DNS_TIMEOUT`).
3. **Hostname Resolution in `/bin/ping` (`/bin/ping`)**:
   - Retains exact existing behavior for numeric IPv4 addresses (`PING <ip> (32 data bytes)`).
   - Non-numeric arguments are resolved via `dns_resolve_ipv4()`.
   - Banner displays `PING <hostname> (<ip>) (32 data bytes)` and subsequent reply lines print `32 bytes from <ip>: icmp_seq=N time=... ms`.
   - Unresolvable hostnames report `ping: cannot resolve <name>` to stderr with exit code 1.
4. **Physical Acceptance (Dell Latitude 5590)**:
   - Verified on bare-metal Dell Latitude 5590: `ping` with domain hostname resolution confirmed working end-to-end with live DNS lookups and subsequent ICMP echo exchanges.

---

## 5. DNS Server Display in `/bin/ifconfig` & Centralized `resolv_conf_path` (2026-10-04)

1. **Centralized `resolv_conf_path()` (`user/resolv_conf.h`)**:
   - Single source of truth returning the active `resolv.conf` path:
     - `/tmp/resolv.conf` if it exists.
     - Else `/mnt/.fortress/resolv.conf` if it exists.
     - Else `/tmp/resolv.conf` (default write target).
   - Uniform callers:
     - `user/dns.c`: parses nameservers via `resolv_conf_path()`.
     - `user/ifup.c`: writes positional DNS arguments (`dns1`, `dns2`) to `resolv_conf_path()`.
     - `user/ifconfig.c`: reads configured nameservers via `resolv_conf_path()`.
   - Eliminates hardcoded paths across userspace networking tools.

2. **In-Memory RAM `/tmp` Directory (`src/fs/vfs.c`)**:
   - Registered writable `/tmp` directory node during `vfs_init()`.
   - Supports creating, writing, and truncating files in memory without requiring a mounted disk or partition.
   - Guarantees `open("/tmp/resolv.conf", O_CREAT | O_WRONLY | O_TRUNC)` succeeds even on read-only boot media.

3. **Output Formatting in `/bin/ifconfig`**:
   - Displays configured nameservers comma-separated immediately after `gateway` and before `mtu`:
     ```text
     eth0  HWaddr c8:f7:50:0e:35:80
           inet 192.168.0.168/24  netmask 255.255.255.0  broadcast 192.168.0.255
           gateway 192.168.0.1
           dns 1.1.1.1, 8.8.8.8
           mtu 1500
           link UP
           RX 1234  TX 567
     ```
   - Edge cases:
     - No servers configured (or file unreadable): `dns` line is completely omitted.
     - Single server: `dns 1.1.1.1` (no trailing comma).
     - Multiple servers: `dns 1.1.1.1, 8.8.8.8`.

4. **Verification & Physical Acceptance (Dell Latitude 5590)**:
   - Host unit tests (`make test-net-ifconfig-host`): PASS under ASan/UBSan.
   - QEMU integration tests (`make test-net-ifconfig`, `make test-net-ifup`): PASS under both BIOS and UEFI.
   - Dell Latitude 5590 bare-metal acceptance: user verified `ifup 192.168.0.x/24 192.168.0.1 1.1.1.1 8.8.8.8` followed by `ifconfig` displaying `dns 1.1.1.1, 8.8.8.8`.

