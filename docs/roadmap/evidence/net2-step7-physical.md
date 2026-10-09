# NET-2 Step 7 — physical DNS acceptance

Date: 2026-10-02
Machine: Dell Latitude 5590
I219: 8086:15D7
MAC: c8:f7:50:0e:35:80
Dell IP: 192.168.0.168
Gateway: 192.168.0.1
Peer: Windows, 192.168.0.153 (Wi-Fi)
DNS fixture: scripts/dns_lan_server.py --bind 192.168.0.153 --answer 192.168.0.153
Echo server: scripts/tcp_echo_server.py --bind 192.168.0.153 --port 7777
Capture filter: ip.addr == 192.168.0.168 && (udp.port == 53 || tcp.port == 53)

## Cases

- service.test -> Address: 192.168.0.153 (UDP A)
- alias.test -> Name: service.test, Address: 192.168.0.153 (CNAME chain)
- missing.test -> nslookup: DNS_NXDOMAIN, exit 1
- tcp.test -> Address: 192.168.0.153 after UDP TC -> TCP fallback
- stall.test -> nslookup: DNS_TIMEOUT, exit 1, bounded recovery
- hostname nc -> `echo fortress-dns | nc -s 192.168.0.153 service.test 7777`
  echoed successfully, prompt returned

Every case followed by `ping -c 1 192.168.0.1` PASS and a usable shell.

## Evidence

- dns-physical-2026-10-02.pcapng (Wireshark capture on the peer)
- Dell screen photographs (Cases 1-5 and hostname nc)
- DNS fixture log
- Echo server log: nc-hostname.peer.stdout

## Notes

- The DNS fixture requires UDP and TCP port 53 on the peer, scoped to the
  Dell's IP.
- The hostname nc case requires the peer's TCP echo server running on 7777
  alongside the DNS fixture on 53.
- The TCP cases (tcp.test, stall.test, hostname nc) require the 120-second
  reboot quiet period to have ended.
- No driver, kernel, scheduler, signal, lock-rank, timer-hook, DMA or
  transport behavior was changed during this physical session.
