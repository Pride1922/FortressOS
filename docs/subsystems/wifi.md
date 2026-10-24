# FortressOS — Wireless Subsystem (802.11 / WPA2-PSK / iwlwifi)

## 1. Overview & Architectural Scope

The FortressOS Wireless Subsystem provides a native, freestanding IEEE 802.11 MAC management frame engine, WPA2-PSK (AES-128-CCMP) 4-Way Handshake state machine, and hardware driver integration for Intel Dual Band Wireless-AC 8265 (Dell Latitude 5590) and Intel Wi-Fi 6E AX211 (Dell Latitude 5530).

The subsystem is fully integrated into the polymorphic `net_dev_t` device layer as interface `wlan0`. Network packet flows, service loops, and socket handoffs seamlessly coexist with wired Gigabit Ethernet (`e1000`/`e1000e`) and USB Gigabit Ethernet (`rtl8153`).

```
+--------------------------------------------------------------------------+
|                      Userland (Ring 3) CLI Tools                         |
|     /bin/wifi (scan, connect, disconnect, status, save, auto)            |
|     /etc/network/wlan.conf (mode 0600 root credential store)             |
+--------------------------------------------------------------------------+
                                    | SYS_WIFI (syscall 87)
+--------------------------------------------------------------------------+
|               Wi-Fi Manager (src/net/wlan/wlan_mgr.c)                    |
|  - Non-blocking asynchronous connection state machine                    |
|  - Autonomous frame retransmission & 6s fail-closed timeout             |
|  - Multi-BSSID best-signal selection & autonomous background roaming    |
|  - Fast-Path PMK Caching (sub-millisecond instant reconnect)             |
|  - BSS Scan Table (deduplicated, sorted by RSSI)                         |
|  - Polymorphic net_dev_t binding (wlan0)                                 |
+--------------------------------------------------------------------------+
          |                                    |
          v                                    v
+-------------------------------+   +--------------------------------------+
| 802.11 MAC Engine & Bridging  |   | WPA2-PSK 4-Way Handshake Engine     |
| (src/net/wlan/net_80211.c)    |   | (src/net/wlan/wlan_wpa2.c)           |
| - 802.3 <-> 802.11 LLC/SNAP   |   | - PBKDF2-HMAC-SHA1 (4096 rounds)     |
| - Beacon/Probe parsing        |   | - IEEE 802.11i PRF-512 PTK / KCK     |
| - Auth / Assoc frame builders |   | - AES-128-CCMP frame crypt / MIC     |
| - Deauth / Disassoc handlers  |   | - 48-bit Packet Number (PN) anti-rep |
+-------------------------------+   +--------------------------------------+
                                    |
+--------------------------------------------------------------------------+
|                Intel PCIe iwlwifi Driver (src/drivers/iwlwifi.c)         |
|  - AC 8265 (PCI 8086:24FD) & AX 211 (PCI 8086:51F0 / 8086:51F1)        |
|  - CSR Register / Transmit / Receive Ring Queues                         |
|  - Official Intel Microcode (iwlwifi-8265-36.ucode.gz, 1.6 MiB stripped) |
|  - Firmware MVM UMAC Scan, Binding Context, Station Add/Modify           |
|  - DTIM Power Management (10ms idle timeouts, skip_dtim_periods = 1)     |
+--------------------------------------------------------------------------+
```

---

## 2. Kernel Invariants & Execution Model

1. **Non-Blocking Syscall Contract**:
   `SYS_WIFI_CMD_CONNECT` initiates connection setup, validates parameters, looks up target BSSID, sets up PMK, prepares firmware station context, transmits the initial Open Authentication Request (Seq 1), and immediately returns `0`. The syscall **never spins or sleeps in kernel space**.
2. **Autonomous Background Progression**:
   Incoming frames are drained and dispatched in `wlan_service_link()` by `net_worker` on CPU 0. Transitions from `AUTHENTICATING` $\to$ `ASSOCIATING` $\to$ `4-WAY HANDSHAKE` $\to$ `CONNECTED` occur purely event-driven.
3. **Retransmissions and Fail-Closed Timeout**:
   - If an Auth Request or Assoc Request frame is dropped over the air, `wlan_service_link()` automatically retransmits after 1.5 seconds (up to 3 retries).
   - If a connection attempt does not reach `WLAN_STATE_CONNECTED` within 6.0 seconds, the state machine fails closed to `WLAN_STATE_FAILED`, wipes sensitive handshake keys, and executes hardware station deauthentication (`s_hw_ops->deauth()`).
4. **Lock Discipline & Stack Limits**:
   - Wi-Fi Manager lock `s_wlan_lock` is rank 1. Driver lock `s_iwl_dev.lock` is rank 2. Locks are always acquired in ascending rank order.
   - `s_wlan_lock` is released **before** calling driver hooks (`prepare_auth`, `deauth`, `send_raw_80211`).
   - Every function respects the Gate G1 stack budget ($\le 1024$ bytes). Packet buffers are zero-heap, using preallocated `pbuf_t` pools and hardware rings.

---

## 3. High-Performance Features

### Fast-Path PMK Caching
Deriving the 256-bit Pairwise Master Key (PMK) via `PBKDF2-HMAC-SHA1` requires 4096 rounds of SHA-1, taking ~150–250 ms of CPU execution. FortressOS retains the derived PMK in memory alongside the SSID and passphrase hash. Subsequent reconnections (or roaming reassociations) skip the computation entirely, establishing authentication in sub-millisecond time.

### Multi-BSSID Best Signal Selection
When connecting to an ESSID with multiple access points (e.g. enterprise or mesh deployments with dual-band 2.4 GHz and 5 GHz radios), `wlan_bss_table_find()` automatically evaluates all BSSIDs and selects the access point with the strongest RSSI.

### Autonomous Background Roaming Engine
While connected, `wlan_service_link()` monitors beacon signal strength in real time:
- **Trigger Condition**: Connected RSSI drops below `-75 dBm`.
- **Candidate Selection**: Searches `s_bss_table` for another BSSID with the same SSID.
- **Hysteresis Threshold**: Candidate must have $\ge +15\text{ dBm}$ stronger signal than current link (preventing ping-pong thrashing).
- **Execution**: Deauthenticates from weak AP, updates hardware channel and station context, and completes authentication/association on the stronger AP using cached PMK.

### DTIM Power Management
Upon entering `WLAN_STATE_CONNECTED`, the Intel iwlwifi driver configures the firmware power table (`IWL_HCMD_MAC_PM_POWER_TABLE`):
- `skip_dtim_periods = 1`: Listens to every 2nd DTIM beacon.
- `rx_data_timeout = 10000` / `tx_data_timeout = 10000`: 10 ms idle wake windows.
- Transitions back to continuous awake (`flags = 0`) during deauthentication or directed active scanning.

### Hardened Credential & Profile Storage
`/etc/network/wlan.conf` stores saved wireless networks in either INI block format or inline `network=SSID:PSK[:ADDR[:GW[:DNS]]]` format. Saved files are created with strict mode `0600` (read/write only by root) via `SYS_OPEN` and `SYS_FCHMOD`, preventing unprivileged users from reading stored network passphrases.

Each saved network profile supports independent IP configuration:
- **Default (DHCP)**: If no static address is specified, the system defaults to DHCP via `/sbin/dhcpc`.
- **Static IP**: If an address is configured (e.g. `192.168.1.150/24`), `wifi` configures the link statically via `/sbin/ifup --no-save` upon connection and writes the DNS servers to `/etc/network/resolv.conf`.

---

## 4. Userland Tooling (`/bin/wifi`)

```bash
wifi scan                                   # Scans all 802.11 channels with an animated progress bar
wifi list                                   # Displays table of saved networks, IP mode, address, gateway, DNS
wifi connect <SSID> [PSK] [--static ..]     # Connects to network (auto-applies saved profile or CLI static flags)
wifi save <SSID> <PSK> [--static <IP/pfx>]  # Persists network credentials & IP config to /etc/network/wlan.conf (0600)
wifi static <SSID> <IP/pfx> [GW] [DNS]      # Sets or updates static IP on a saved network profile
wifi static <SSID> dhcp                     # Reverts a saved network profile back to DHCP
wifi auto                                   # Auto-connects to best saved network by RSSI and applies its IP config
wifi disconnect                             # Disconnects and releases hardware station context
wifi status                                 # Displays wlan0 MAC, BSSID, channel, RSSI, and link state
```

### Seamless Ethernet / Wi-Fi Handover Architecture
1. **Cable Insertion (Wired Link UP)**:
   - Kernel prioritizes wired interfaces (`rtl8153` > `e1000` > `wlan0`).
   - Upon wired link online, `net_service_active_interface` automatically triggers `wlan_mgr_disconnect()`, sending an 802.11 Deauth to the AP and putting the Wi-Fi radio to idle sleep.
   - Background `/sbin/dhcpc` daemon detects MAC change, flushes old leases, checks `/etc/network/network.conf` for wired static IP (or requests a fresh wired DHCP lease), and writes `/etc/network/resolv.conf`.
2. **Cable Removal (Wired Link DOWN)**:
   - When the Ethernet cable is unplugged, `/sbin/dhcpc` detects carrier loss, enters `STATE_DISCONNECTED`, and periodically invokes `/bin/wifi auto`.
   - `wifi auto` scans range, selects the saved network with the strongest RSSI, executes the 4-way WPA2 handshake, and configures either static IP or DHCP based on that network's saved profile.

---

## 5. Verification Matrix & Host Test Suites

The subsystem is validated 100% PASS under host sanitizers (ASan/UBSan) and automated build checks:

| Target | Description | Evidence |
| :--- | :--- | :--- |
| `make test-wifi-host` | 16 comprehensive Wi-Fi Manager and state machine tests: initialization, beacon cache, auth req, assoc req, 4-way handshake M1–M4, LLC/SNAP bridging, AES-128-CCMP crypt, PN replay protection, microcode validation, clean disconnect, bad passphrase fail-closed, AP deauth handling, async retry & 6s timeout, PMK caching, and multi-BSSID best-signal selection + autonomous roaming. | **16/16 PASS (100%)** |
| `make test-wlan-mac-host` | 4 802.11 MAC engine tests: Management frame builders, 802.3 $\leftrightarrow$ 802.11 bridging roundtrip, Beacon/Probe response parsing, BSS scan table deduplication & multi-BSSID best RSSI selection. | **4/4 PASS (100%)** |
| `make test-wlan-crypto-host` | 6 cryptographic engine tests: RFC 3174 SHA-1, RFC 2202 HMAC-SHA-1, RFC 6070 PBKDF2 (4096 rounds), IEEE 802.11i PRF-512 PTK derivation, RFC 3394 AES Key Wrap, AES-128-CCMP tamper detection. | **6/6 PASS (100%)** |
| `make test-wlan-wpa2-host` | Complete WPA2-PSK 4-Way Handshake state machine and adversarial gates (replay counters and tampered MIC fail-closed). | **2/2 PASS (100%)** |
