# NETCTL_IFGET and NETCTL_IFSET ABI — NET-3 Runtime Network Configuration

Status: APPROVED SPECIFICATION (2026-10-02) for NET-3 implementation.
Prerequisite: Cold-cable link recovery milestone complete (Dell 5590 physically accepted).

`SYS_NETCTL = 42`.
Existing command: `NETCTL_PING = 1u` (Phase 4, size 48).
New commands:
- `NETCTL_IFGET = 2u` (query interface state, size 64)
- `NETCTL_IFSET = 3u` (set IPv4 configuration, size 32)

Calling convention (fast syscall):
- `RAX = SYS_NETCTL (42)`
- `RDI = command` (`NETCTL_IFGET` or `NETCTL_IFSET`)
- `RSI = user struct pointer`
- `RDX = struct size` (`sizeof(netctl_ifget_t)` or `sizeof(netctl_ifset_t)`)

---

## 1. System Call Dispatch & Precedence

All `SYS_NETCTL` commands are BSP-pinned and follow a strict, unified error precedence:

1. **CPU affinity check**: Caller must be executing on CPU 0 and have `cpu_affinity == 0`.
   - If not: return `SYSCALL_EOPNOTSUPP` (-14) immediately before inspecting user memory.
2. **Command identifier & size check**:
   - If `RDI == NETCTL_IFGET` and `RDX != sizeof(netctl_ifget_t)` (64): return `SYSCALL_EINVAL` (-1).
   - If `RDI == NETCTL_IFSET` and `RDX != sizeof(netctl_ifset_t)` (32): return `SYSCALL_EINVAL` (-1).
   - If `RDI` is unrecognized: return `SYSCALL_EINVAL` (-1).
3. **User range validation (`vmm_validate_user_range`)**:
   - `NETCTL_IFGET`: validate `RSI` for 64 bytes writable (`write=true`). If invalid: return `SYSCALL_EFAULT` (-2).
   - `NETCTL_IFSET`: validate `RSI` for 32 bytes readable (`write=false`). If invalid: return `SYSCALL_EFAULT` (-2).
4. **Copy into stack-local buffer**:
   - Single scalar `memcpy` from user address into kernel stack. No user pointers held.
5. **Field validation**:
   - Input fields validated on stack struct according to command rules. If invalid: return `SYSCALL_EINVAL` (-1).
6. **State & device checks**:
   - Absent controller (`s_if == NULL`): return `SYSCALL_EIO` (-9).
   - Any link state other than `NET_IF_LINK_ONLINE` for `IFSET`: return `SYSCALL_EIO` (-9).

---

## 2. NETCTL_IFGET Layout (Command 2)

Queries interface link state, hardware MAC, MTU, current IPv4 configuration, and monotonic driver packet counters. Returns a fixed 64-byte struct with 8-byte alignment.

```c
typedef struct {
    uint32_t struct_version;   /*  0, 4: must be 1 on input */
    uint32_t reserved;         /*  4, 4: must be 0 on input */
    uint8_t  mac[6];           /*  8, 6: hardware MAC address */
    uint16_t reserved2;        /* 14, 2: 0 on output */
    uint32_t local_ipv4;       /* 16, 4: network byte order; 0 if unconfigured */
    uint32_t netmask_ipv4;     /* 20, 4: network byte order; 0 if unconfigured */
    uint32_t gateway_ipv4;     /* 24, 4: network byte order; 0 if unconfigured */
    uint32_t mtu;              /* 28, 4: interface MTU (typically 1500) */
    uint32_t link_state;       /* 32, 4: NET_IF_LINK_* stable ABI value */
    uint32_t reserved3;        /* 36, 4: padding for 8B alignment; 0 on output */
    uint64_t rx_packets;       /* 40, 8: driver packets received */
    uint64_t tx_packets;       /* 48, 8: driver packets transmitted */
    uint32_t reserved4[2];     /* 56, 8: reserved; 0 on output */
} netctl_ifget_t;
```

### Offset & Alignment Table

| Offset | Field | Type | Bytes | Direction | Rules / Output |
|---|---|---|---|---|---|
| 0 | `struct_version` | `uint32_t` | 4 | in/out | In: must be 1. Out: 1. |
| 4 | `reserved` | `uint32_t` | 4 | in/out | In: must be 0. Out: 0. |
| 8 | `mac[6]` | `uint8_t[6]` | 6 | out | Hardware MAC (e.g. `c8:f7:50:0e:35:80`). |
| 14 | `reserved2` | `uint16_t` | 2 | out | Set to 0. |
| 16 | `local_ipv4` | `uint32_t` | 4 | out | Network byte order (big-endian). |
| 20 | `netmask_ipv4` | `uint32_t` | 4 | out | Network byte order (big-endian). |
| 24 | `gateway_ipv4` | `uint32_t` | 4 | out | Network byte order; 0 if no gateway. |
| 28 | `mtu` | `uint32_t` | 4 | out | Interface MTU. |
| 32 | `link_state` | `uint32_t` | 4 | out | Stable ABI values: `NET_IF_LINK_UNINITIALIZED=0`, `NET_IF_LINK_WAITING=1`, `NET_IF_LINK_ONLINE=2`, `NET_IF_LINK_DOWN=3`, `NET_IF_LINK_FAILED=4`. These are project-defined values, mapped from the driver's internal enum by a stable function. Userspace treats them as opaque and maps to display strings. |
| 36 | `reserved3` | `uint32_t` | 4 | out | Set to 0 (padding for 8-byte alignment). |
| 40 | `rx_packets` | `uint64_t` | 8 | out | Monotonic counter of received packets. |
| 48 | `tx_packets` | `uint64_t` | 8 | out | Monotonic counter of transmitted packets. |
| 56 | `reserved4[2]` | `uint32_t[2]` | 8 | out | Set to 0. |

Size: exactly 64 bytes. Alignment: 8 bytes.

### Compile-Time Static Assertions
```c
_Static_assert(sizeof(netctl_ifget_t) == 64, "netctl_ifget_t size must be 64");
_Static_assert(__builtin_offsetof(netctl_ifget_t, struct_version) == 0,  "offset version");
_Static_assert(__builtin_offsetof(netctl_ifget_t, reserved)       == 4,  "offset reserved");
_Static_assert(__builtin_offsetof(netctl_ifget_t, mac)            == 8,  "offset mac");
_Static_assert(__builtin_offsetof(netctl_ifget_t, reserved2)      == 14, "offset reserved2");
_Static_assert(__builtin_offsetof(netctl_ifget_t, local_ipv4)     == 16, "offset local_ip");
_Static_assert(__builtin_offsetof(netctl_ifget_t, netmask_ipv4)   == 20, "offset netmask");
_Static_assert(__builtin_offsetof(netctl_ifget_t, gateway_ipv4)   == 24, "offset gateway");
_Static_assert(__builtin_offsetof(netctl_ifget_t, mtu)            == 28, "offset mtu");
_Static_assert(__builtin_offsetof(netctl_ifget_t, link_state)     == 32, "offset link_state");
_Static_assert(__builtin_offsetof(netctl_ifget_t, reserved3)      == 36, "offset reserved3");
_Static_assert(__builtin_offsetof(netctl_ifget_t, rx_packets)     == 40, "offset rx_packets");
_Static_assert(__builtin_offsetof(netctl_ifget_t, tx_packets)     == 48, "offset tx_packets");
_Static_assert(__builtin_offsetof(netctl_ifget_t, reserved4)      == 56, "offset reserved4");
```

---

## 3. NETCTL_IFSET Layout (Command 3)

Changes the local IPv4 address, netmask, and default gateway of the running interface at runtime. Returns 0 on success, negative error code on failure.

```c
typedef struct {
    uint32_t struct_version;   /*  0, 4: must be 1 */
    uint32_t reserved;         /*  4, 4: must be 0 */
    uint32_t local_ipv4;       /*  8, 4: network byte order */
    uint32_t netmask_ipv4;     /* 12, 4: network byte order */
    uint32_t gateway_ipv4;     /* 16, 4: network byte order; 0 = no gateway */
    uint32_t flags;            /* 20, 4: must be 0 in v1 */
    uint32_t reserved2[2];     /* 24, 8: must be 0 */
} netctl_ifset_t;
```

### Offset & Alignment Table

| Offset | Field | Type | Bytes | Direction | Rules |
|---|---|---|---|---|---|
| 0 | `struct_version` | `uint32_t` | 4 | in | Must be 1; else `SYSCALL_EINVAL`. |
| 4 | `reserved` | `uint32_t` | 4 | in | Must be 0; else `SYSCALL_EINVAL`. |
| 8 | `local_ipv4` | `uint32_t` | 4 | in | Network byte order. Valid unicast host; else `SYSCALL_EINVAL`. |
| 12 | `netmask_ipv4` | `uint32_t` | 4 | in | Network byte order. Contiguous mask, prefix 1–30; else `SYSCALL_EINVAL`. |
| 16 | `gateway_ipv4` | `uint32_t` | 4 | in | Network byte order. 0 (none) or valid unicast host on-link; else `SYSCALL_EINVAL`. |
| 20 | `flags` | `uint32_t` | 4 | in | Must be 0 in v1; else `SYSCALL_EINVAL`. |
| 24 | `reserved2[2]` | `uint32_t[2]` | 8 | in | Must be 0; else `SYSCALL_EINVAL`. |

Size: exactly 32 bytes. Alignment: 4 bytes.

### Compile-Time Static Assertions
```c
_Static_assert(sizeof(netctl_ifset_t) == 32, "netctl_ifset_t size must be 32");
_Static_assert(__builtin_offsetof(netctl_ifset_t, struct_version) == 0,  "offset version");
_Static_assert(__builtin_offsetof(netctl_ifset_t, reserved)       == 4,  "offset reserved");
_Static_assert(__builtin_offsetof(netctl_ifset_t, local_ipv4)     == 8,  "offset local_ip");
_Static_assert(__builtin_offsetof(netctl_ifset_t, netmask_ipv4)   == 12, "offset netmask");
_Static_assert(__builtin_offsetof(netctl_ifset_t, gateway_ipv4)   == 16, "offset gateway");
_Static_assert(__builtin_offsetof(netctl_ifset_t, flags)          == 20, "offset flags");
_Static_assert(__builtin_offsetof(netctl_ifset_t, reserved2)      == 24, "offset reserved2");
```

---

## 4. Field Validation Order & Semantics

Struct field validation executes on the stack-local copy in the following strict order:

1. **Header validation**:
   - `struct_version == 1`
   - `reserved == 0`
   - `flags == 0`
   - `reserved2[0] == 0 && reserved2[1] == 0`
   - Failure: return `SYSCALL_EINVAL`.

2. **`local_ipv4` validation**:
   - Let `h = ntohl(local_ipv4)`.
   - Must not be `0.0.0.0` (`h == 0`).
   - Must not be loopback (`(h >> 24) == 127`).
   - Must not be 0.x.x.x (`(h >> 24) == 0`).
   - Must not be multicast or reserved (`(h >> 24) >= 224`).
   - Must not be limited broadcast (`h == 0xffffffff`).
   - Failure: return `SYSCALL_EINVAL`.

3. **`netmask_ipv4` validation**:
   - Let `m = ntohl(netmask_ipv4)`.
   - Must be contiguous sequence of 1-bits followed by 0-bits.
   - Prefix length $P = \text{count\_leading\_ones}(m)$.
   - Valid prefix range: $1 \le P \le 30$. (Host masks `/32` and point-to-point `/31` rejected in v1).
   - Inverted mask check: `(m | (m - 1)) == 0xffffffff` or equivalent `~m & (~m + 1) == 0`.
   - Subnet host bits check on `local_ipv4`:
     - Subnet network ID: `(h & ~m) == 0` $\rightarrow$ rejected.
     - Subnet broadcast ID: `(h & ~m) == ~m` $\rightarrow$ rejected.
   - Failure: return `SYSCALL_EINVAL`.

4. **`gateway_ipv4` validation**:
   - If `gateway_ipv4 == 0`: gateway is unconfigured; valid.
   - If `gateway_ipv4 != 0`:
     - Let `gw = ntohl(gateway_ipv4)`.
     - Must be valid unicast host (not 0, not loopback, not multicast $\ge 224$, not subnet broadcast).
     - Must **not** equal local IP (`gateway_ipv4 != local_ipv4`). A host cannot be its own gateway.
     - Must be **on-link**: `(gw & m) == (h & m)`.
   - Failure: return `SYSCALL_EINVAL`.

5. **Hardware & link state check**:
   - Sample driver link state via the lockless state accessor `e1000_link_state_abi(s_if)`.
   - Must have `s_if != NULL` and link state `NET_IF_LINK_ONLINE`. Any state other than `NET_IF_LINK_ONLINE` (covering `WAITING`, `DOWN`, `FAILED`, and `UNINITIALIZED` uniformly) or an absent controller returns `SYSCALL_EIO`. You cannot change the IP of an interface that is not online.

---

## 5. Synchronization & Atomic Update (`net_set_config`)

To prevent multi-layer protocol staleness:

```c
void net_set_config(const net_config_t *new_cfg);
```

### Execution Flow:
1. Syscall executes purely on BSP.
2. Validates struct on kernel stack (no lock held).
3. Checks link state via the new lockless state accessor `e1000_link_state_abi(s_if)`, matching `e1000_network_online()`. Any state other than `NET_IF_LINK_ONLINE` returns `SYSCALL_EIO`.
4. Acquires Rank-1 `g_net_stack_lock` with `spin_lock_irqsave`.
5. Updates configuration across all four protocol layers atomically:
   - `s_config` in `src/net/net.c`.
   - Resets ARP cache via `arp_cache_init(&s_arp_cache)`.
   - `s_cfg` in `src/net/net_ipv4.c` (updates `local_ip`, `prefix`, `gateway`).
   - `s_local` in `src/net/net_socket.c`.
   - `local_ip` in `src/net/net_tcp.c`.
6. Releases `g_net_stack_lock` with `spin_unlock_irqrestore`.
7. Returns `SYSCALL_SUCCESS` (0).

### Invariants:
- `g_net_stack_lock` is Rank 1. It **never nests** with `g_net_dev_lock`, `g_socket_table_lock`, `tcp_endpoints`, or any scheduler lock.
- User pointers are never touched while holding `g_net_stack_lock`.
- No memory allocation is performed during `IFSET`.
- Existing TCP/UDP sockets are not terminated. Packets addressed to or from the old IP will fail naturally on timeout or route failure.
- New TCP/UDP connections immediately bind to and encode headers using the new local IP.
- The TCP salt and ISN clock (`src/net/net_tcp.c`) are deliberately not updated by IFSET. They are per-boot state, not per-configuration state. Recomputing the salt mid-boot would risk ISN collisions with existing connections. The salt's purpose is cross-boot unpredictability, not tracking the local IP.

---

## 6. Return Codes

| Code | Value | Name | Condition |
|---|---|---|---|
| 0 | 0 | `SYSCALL_SUCCESS` | Command completed successfully. |
| -1 | -1 | `SYSCALL_EINVAL` | Unknown command, wrong struct size, bad version, flags $\ne 0$, reserved fields $\ne 0$, non-unicast IP, invalid netmask (not prefix 1–30), off-link gateway, or gateway == local IP. |
| -2 | -2 | `SYSCALL_EFAULT` | Inaccessible or invalid user memory pointer range. |
| -9 | -9 | `SYSCALL_EIO` | Network controller absent (`s_if == NULL`), or interface link state is not `NET_IF_LINK_ONLINE` during `IFSET`. |
| -14 | -14 | `SYSCALL_EOPNOTSUPP` | System call invoked from Application Processor (AP) or thread not pinned to BSP (CPU 0). |
