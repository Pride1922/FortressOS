# FortressOS — Wireless Subsystem (802.11 / WPA2-PSK / iwlwifi)

## 1. Overview & Architectural Scope

2026-11-02 cold inbound ARP follow-up: supplied-neighbor testing isolates
failing initial ARP from working unicast ICMP on the Dell. Intel MAC_CONTEXT
now requests raw encrypted unicast/group delivery for FortressOS software
crypto. Actual command-byte regression passes; physical confirmation remains
pending. [Evidence, API reference and limits](../roadmap/wifi-cold-arp.md).

2026-11-02 installed idle/reboot follow-up: reproduced the production selector
evicting Wi-Fi for a disconnected Ethernet device during transient offline
states. The selector now retains Wi-Fi until another link is actually online,
and repeated `wifi auto` no longer restarts live/in-flight authentication.
Host regressions exercise the actual selection path that previous fixtures
bypassed. [Cause, tests and physical boundary](../roadmap/wifi-interface-flapping.md).

2026-11-02 installed-root reconnect fix: missing credentials no longer turn a
protected beacon into an open link; CLI save/reconnect snapshots prevent
self-erasure of saved SSIDs/passwords, and checked atomic profile writes retain
previous configurations on failure. Existing enabled `wifi.service` runs
`wifi auto` at boot. Host verification passes; guest/physical reboot acceptance
remains pending. [Changes, evidence and limits](../roadmap/wifi-profile-reconnect.md).

2026-10-28 GUI connection follow-up: a selected BSSID uses its own cached
channel/RSSI/beacon interval, rather than metadata from the currently strongest
same-SSID AP. The desktop also performs the CLI's firmware preparation before
scan/join using a reaped background child. The user reports terminal connection
works while GUI association fails; these source differences are fixed and
host/QEMU verified, with Dell GUI association retest still required.
[Evidence and scope](../roadmap/gui-wifi-connect.md).

### CPU-0 management ownership (issue #76, 2026-10-28)

All five SYS_WIFI operations now use retained copied requests through the
registered CPU-0 network worker, from BSP and AP callers alike. Status is a
pure WLAN snapshot; lazy hardware initialization, scan collection, firmware
loading, connect and disconnect execute on the owner. Four copied records and
one at-most-4-MiB firmware allocation bound management storage. Saturation
returns EAGAIN. Request credentials are wiped after execution or owner cleanup;
no user pointer or caller stack crosses ownership. The existing public syscall
access policy is unchanged: SYS_WIFI itself has no capability gate. The desktop
still requires its existing root display authorization.

The temporary compositor CPU-count restriction is removed after actual-code
ASan/UBSan/TSan and BIOS/UEFI × SMP=1/4/8 ownership verification. QEMU uses an
explicit mock radio and actual Ring 3 calls on every CPU. Real Intel radio/DMA,
on-channel timing and cable handover remain Dell acceptance items. See the
[implementation audit, evidence and limits](../roadmap/wifi-cpu0-owner.md).

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
   `SYS_WIFI_CMD_CONNECT` initiates connection setup, validates parameters, looks up target BSSID, sets up PMK, prepares firmware station context, transmits the initial Open Authentication Request (Seq 1), and returns `0` after that bounded setup. The caller waits only for the copied owner transaction, not for association or WPA2 completion. PMK derivation and existing hardware command deadlines remain bounded synchronous setup work on the owner. The former 600,000-iteration RX pump is replaced by at-most-64-frame owner turns, yielding between turns while connection setup remains active; this hardware timing change was explicitly approved for Dell retesting.
2. **Autonomous Background Progression**:
   Incoming frames are drained and dispatched in `wlan_service_link()` by `net_worker` on CPU 0. Transitions from `AUTHENTICATING` $\to$ `ASSOCIATING` $\to$ `4-WAY HANDSHAKE` $\to$ `CONNECTED` occur purely event-driven.
3. **Retransmissions and Fail-Closed Timeout**:
   - If an Auth Request or Assoc Request frame is dropped over the air, `wlan_service_link()` automatically retransmits after 1.5 seconds (up to 3 retries).
   - If a connection attempt does not reach `WLAN_STATE_CONNECTED` within 6.0 seconds, the state machine fails closed to `WLAN_STATE_FAILED`, wipes sensitive handshake keys, and executes hardware station deauthentication (`s_hw_ops->deauth()`).
4. **Lock Discipline & Stack Limits**:
   - Wi-Fi Manager lock `s_wlan_lock` and copied-value lock `wifi_handoff_values` are disjoint rank 1 locks. Driver lock `s_iwl_dev.lock` is rank 2. Locks are always acquired in ascending rank order.
   - `wifi_handoff_values` is released before any WLAN/driver work, heap operations, user copies, waits or wakes. `prepare_auth` and hardware `deauth` run outside `s_wlan_lock`; existing raw TX paths may hold WLAN rank 1 while acquiring driver rank 2. Status invokes no driver hooks.
   - Functions respect the Gate G1 stack budget ($\le 1024$ bytes), with the existing explicitly authorized `wpa2_sm_rx_eapol` exception. Packet buffers are zero-heap, using preallocated `pbuf_t` pools and hardware rings.

---

## 3. High-Performance Features

### Fast-Path PMK Caching
Deriving the 256-bit Pairwise Master Key (PMK) via `PBKDF2-HMAC-SHA1` requires 4096 rounds of SHA-1, taking ~150–250 ms of CPU execution. FortressOS retains the derived PMK in memory alongside the SSID and cached passphrase (the current implementation stores that comparison value directly, not as a hash). Subsequent reconnections (or roaming reassociations) skip the computation entirely, establishing authentication in sub-millisecond time.

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

### Dynamic Group Cipher Negotiation & RSN IE Consistency
`net_80211_parse_beacon_or_probe()` extracts the AP's advertised Group Data Cipher Suite (`00-0F-AC:2` TKIP, `00-0F-AC:4` CCMP) and RSN capabilities.
`net_80211_build_assoc_req_ext()` and `wpa2_sm_rx_eapol()` (Message 2) dynamically mirror both the AP's group cipher and Management Frame Protection Capable (MFPC, bit 7 / `0x0080`) bit across both the Association Request and EAPOL Message 2 RSN IEs. Both RSN IEs match byte-for-byte, strictly eliminating both AP Association Rejection Status Code 53 (`0x35`, Robust Management Policy Violation required by Android WPA2/WPA3-Personal transition mode) and AP Deauthentication Reason Code 17 (`0x0011` / `WLAN_REASON_IE_IN_4WAY_DIFFERS`).

### Wi-Fi DHCP Unicast Delivery
When acquiring an IP lease over `wlan0`, `/sbin/dhcpc` clears `DHCP_FLAGS_BROADCAST` to request unicast delivery per RFC 2131. Unicast DHCPOFFER and DHCPACK frames are encrypted under the station's Pairwise Temporal Key (AES-128-CCMP), preventing packet drop on APs using TKIP group ciphers. The IPv4 network stack (`src/net/net_ipv4.c`) permits inbound UDP port 68 datagrams destined to the newly offered IP before local interface IP configuration.
`wlan_mgr.c` emits real-time serial diagnostic telemetry for 802.11 Auth Response status, Assoc Response status, Deauth/Disassoc reason codes, and EAPOL M1–M4 handshake transitions.

### Full-Spectrum 5 GHz Channel Scanning
`s_scan_channels` in `iwlwifi.c` covers 37 standard 802.11 channels:
- **2.4 GHz**: Channels 1–13
- **5 GHz UNII-1**: Channels 36, 40, 44, 48
- **5 GHz UNII-2 (DFS)**: Channels 52, 56, 60, 64
- **5 GHz UNII-2 Extended (DFS)**: Channels 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140
- **5 GHz UNII-3**: Channels 149, 153, 157, 161, 165
This ensures complete discovery of mobile hotspots (e.g. Google Pixel 9 Pro defaulting to UNII-3/DFS) and dual-band access points.

### Hardened Credential & Profile Storage (with Desktop GUI Auto-Persistence)
`/etc/network/wlan.conf` stores saved wireless networks in either INI block format or inline `network=SSID:PSK[:ADDR[:GW[:DNS]]]` format. Saved files are created with strict mode `0600` (read/write only by root) via `SYS_OPEN` and `SYS_FCHMOD`, preventing unprivileged users from reading stored network passphrases.

The desktop compositor (`desktop.rs` & `sys.rs`) registers `SETTING_WLAN = 3` through the root settings broker and invokes `wireless_save_network()` upon initiating Wi-Fi connections. Network profiles configured in the GUI are automatically persisted and deduplicated to `/etc/network/wlan.conf`, allowing Watchtower's `wifi.service` (`/bin/wifi auto`) to establish network connectivity automatically on boot.

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
wifi forget [SSID]                          # Removes saved network profile (or currently active network)
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

### Open / Public Wi-Fi Network Support (Zero Passphrase)
When connecting to an open access point (no WPA2/SAE advertisement and no Privacy capability bit), `wlan_mgr_connect()` sets `s_target_is_open = true`, omits the RSN Information Element (IE 48) and clears the Privacy capability bit (`0x0010`) in the Association Request, and skips `WLAN_STATE_4WAY_HANDSHAKE` upon receiving Association Response `SUCCESS` (`status == 0`), transitioning directly to `WLAN_STATE_CONNECTED` with immediate port authorization. Data frames on open links are encapsulated and decapsulated without `IEEE80211_FCTL_PROTECTED`, while secured links strictly reject unencrypted data frames.

### TKIP Group Frame Decryption (Mixed-Mode AP Broadcast ARP / Multicast)
On mixed-mode routers advertising `Group Cipher = 00-0F-AC:2` (TKIP) with `Pairwise Cipher = 00-0F-AC:4` (AES-CCMP), `wpa2_sm_rx_eapol()` unpacks the full 32-byte TKIP Group Temporal Key (`16B Temporal Key + 8B AP Tx Michael MIC Key + 8B AP Rx Michael MIC Key`) from EAPOL Message 3. Inbound broadcast/multicast data frames (`addr1[0] & 0x01`) are decrypted via `wlan_tkip_decrypt_frame()` using IEEE 802.11i TKIP Phase 1 & Phase 2 key mixing, RC4 stream cipher, CRC-32 ICV verification, Michael MIC verification over `DA || SA || Priority || 0x000000 || MSDU`, WEPSeed validation, and a 64-packet sliding TSC replay window.

### Desktop Compositor Wi-Fi GUI Polishing
The desktop compositor (`user/compositor/src/sys.rs`, `ui.rs`, `desktop.rs`) exposes `sys::wireless_scan_results()` to read the kernel BSS scan cache without blocking on a fresh radio sweep when opening **Network & Wi-Fi**, renders 4-level signal bars ($\ge -50\text{ dBm} = 4\text{ bars}$, $\ge -65\text{ dBm} = 3\text{ bars}$, $\ge -75\text{ dBm} = 2\text{ bars}$, $< -75\text{ dBm} = 1\text{ bar}$), displays a padlock icon for secured networks (`WPA2-PSK`, `WPA3-SAE`, `WPA2/WPA3`) vs `Open` badge for open hotspots, connects with one click on open networks, and presents a **Join Wi-Fi Network** password prompt modal card for secured networks.

### 802.11n (HT) & 802.11ac (VHT) Throughput Scaling
`net_80211_parse_beacon_or_probe()` parses HT Capabilities (IE 45), HT Operation (IE 61), VHT Capabilities (IE 191), and VHT Operation (IE 192), and `net_80211_build_assoc_req_full()` advertises 2x2 MIMO HT/VHT capabilities (20/40/80 MHz, Short GI, LDPC, 64K A-MPDU factor, VHT MCS 0–9 NSS 1–2). Upon association, `iwlwifi_configure_ht_vht()` updates the Intel MVM station context (`IWL_STA_FLG_MAX_AGG_SIZE_64K`, `IWL_STA_FLG_FAT_EN_*`, `IWL_STA_FLG_MIMO_EN_MIMO2`, `rx_ba_window = 64`) and populates `IWL_HCMD_LQ` with VHT/HT MCS rate-n-flags. Incoming `ADDBA Request` action frames (Category 3, Action 0) are answered with `ADDBA Response` (Status 0, 64-buffer window).

### WPA3-Personal (SAE / Dragonfly Handshake)
`wlan_crypto.c`, `net_80211.c`, `wlan_wpa2.c`, and `wlan_mgr.c` implement RFC 7664 / IEEE 802.11-2020 Simultaneous Authentication of Equals (SAE) over elliptic curve group 19 (NIST P-256 / `secp256r1`) with constant-step Montgomery ladder scalar multiplication, hunting-and-pecking Password Element (PWE) derivation, anti-reflection verification, SAE Commit (`Auth Alg = 3, Seq = 1`) and SAE Confirm (`Auth Alg = 3, Seq = 2`) exchange, `KDF-SHA-256` PTK derivation, `HMAC-SHA256-128` EAPOL Key MICs (Descriptor Version 0), and 16-byte `PMKID` inclusion in the Association Request and EAPOL Message 2 RSN IEs (`MFPC | MFPR`).

### Multi-Vendor `wlan_mac_ops_t` Driver Architecture (`iwlwifi` & `rtw88`)
`wlan_mgr.h` defines the hardware-portable `wlan_mac_ops_t` interface and `wlan_register_driver()` registration table supporting up to 8 compiled-in wireless drivers. Alongside Intel `iwlwifi`, `src/drivers/rtw88.c` provides driver binding and PCIe enumeration for Realtek 802.11ac/ax NICs (`RTL8821CE` `10EC:C821`, `RTL8822BE` `10EC:B822`, `RTL8822CE` `10EC:C822`, `RTL8852AE` `10EC:8852`, `RTL8852BE` `10EC:B852`) and Qualcomm Atheros PCIe NICs (`QCA6174` `168C:003E`, `QCA9377` `168C:0042`).

---

## 5. Verification Matrix & Host Test Suites

The subsystem is validated 100% PASS under host sanitizers (ASan/UBSan) and automated build checks:

| Target | Description | Evidence |
| :--- | :--- | :--- |
| `make test-wifi-owner-host` / `make test-wifi-owner` | Actual copied SYS_WIFI/handoff/WLAN code under ASan/UBSan/TSan; isolated BIOS/UEFI × SMP=1/4/8 with real Ring 3 BSP/AP entry, mock radio owner assertions, saturation and cancellation/death cleanup. | **PASS; Intel physical acceptance pending** |
| `make test-wifi-host` | 21 comprehensive Wi-Fi Manager and state machine tests: initialization, beacon cache, auth req, assoc req, 4-way handshake M1–M4, LLC/SNAP bridging, AES-128-CCMP crypt, PN replay protection, microcode validation, clean disconnect, bad passphrase fail-closed, AP deauth handling, async retry & 6s timeout, PMK caching, multi-BSSID best-signal selection + autonomous roaming, Open Wi-Fi connection + unencrypted TX/RX, HT/VHT negotiation + ADDBA Response, Mixed-Mode TKIP group broadcast ARP decryption, WPA3-Personal SAE Commit-Confirm flow, and multi-vendor `rtw88` driver registration. | **21/21 PASS (100%)** |
| `make test-wlan-mac-host` | 6 802.11 MAC engine tests: Management frame builders, 802.3 $\leftrightarrow$ 802.11 bridging roundtrip, Beacon/Probe response parsing, BSS scan table deduplication & multi-BSSID best RSSI selection, Open/HT/VHT/SAE/ADDBA frame builders, and HT/VHT/SAE beacon parsing. | **6/6 PASS (100%)** |
| `make test-wlan-crypto-host` | 9 cryptographic engine tests: RFC 3174 SHA-1, RFC 2202 HMAC-SHA-1, RFC 6070 PBKDF2 (4096 rounds), IEEE 802.11i PRF-512 PTK derivation, RFC 3394 AES Key Wrap, AES-128-CCMP tamper detection, TKIP group encryption/decryption + Michael MIC + replay window, FIPS 180-4 SHA-256 + RFC 4231 HMAC-SHA-256 + KDF-SHA-256, and WPA3-SAE NIST P-256 PWE + Commit-Confirm + anti-reflection. | **9/9 PASS (100%)** |
| `make test-wlan-wpa2-host` | 4 WPA2-PSK & WPA3-SAE 4-Way Handshake tests: full WPA2 handshake, adversarial replay/MIC gates, dynamic Group Cipher & PMF RSN IE mirroring, and WPA3-SAE SHA-256 4-Way Handshake + 32-byte TKIP GTK KDE extraction. | **4/4 PASS (100%)** |
| `make test-wifi-stack` | Static stack frame gate across all 8 Wi-Fi & driver translation units (`net_wifi_owner.c`, `wlan_mgr.c`, `net_80211.c`, `wlan_wpa2.c`, `wlan_crypto.c`, `iwlwifi.c`, `iwl_health.c`, `rtw88.c`). | **PASS (221 functions, $\le 1024$ B budget)** |
