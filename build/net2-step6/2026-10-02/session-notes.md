# NET-2 Step 6 physical session — 2026-10-02

Machine: Dell Latitude 5590
I219: 8086:15D7
MAC: c8:f7:50:0e:35:80
Dell IP: 192.168.0.168
Gateway: 192.168.0.1
Peer: Windows, 192.168.0.153 (Wi-Fi)
Boot cmdline: net=192.168.0.168/24,192.168.0.1 verbose
Capture: Wireshark on the peer, Wi-Fi adapter

## Cases

- **A — guest client, peer echo server.** PASS. Guest sent
  "fortress-tcp-physical", received the same bytes back, prompt returned,
  follow-up ping 1/1. Wire: SYN, SYN-ACK, ACK, 22-byte PSH, echo,
  Window Update, FIN/ACK/FIN, ZeroWindow. MSS=1460 both sides. Clean
  four-way teardown, no RST, no retransmission.
- **B — guest client, real HTTP server (Python http.server).** PASS.
  Guest sent `GET / HTTP/1.0`, received `HTTP/1.0 200 OK` with
  Content-Length 21 and body "Hello from the peer". Prompt returned,
  follow-up ping 1/1. MSS=1460 both sides.
- **C — peer client, guest listener (`nc -l 7777`).** PASS after the
  `nc -l` tty-skip fix. Guest printed the 21-byte payload
  ("fortress-tcp-inbound\n"), returned to the prompt, follow-up ping 1/1.
  Wire: clean handshake, peer sent data+FIN back-to-back, guest ACKed
  data (Ack=22, Win=8171), then post-FIN ZeroWindow, guest FIN, peer ACK.
  No RST, no retransmission.
- **D — peer abort with RST (SO_LINGER 0).** PASS. Guest's `nc` returned
  an error and the shell stayed usable; follow-up ping 1/1. Wire: guest
  sent 20 bytes ("fortress-reset-test\n"), then FIN; peer ACKed both and
  sent RST, ACK 521 ms later. Guest handled the RST without hanging.

## Findings

- Ethernet cable must be connected before boot; the I219 PHY does not
  renegotiate a link that comes up after the driver initialized. Reboot
  with the cable connected is the workaround. This is the documented
  "no hot-plug" behavior.
- `nc -l` originally read stdin before the socket. On the guest there
  is no `/dev/null`, so `nc -l <port>` blocked on the terminal until
  Ctrl-C. Fixed by detecting tty stdin via `TERM_ISATTY` and skipping
  the stdin read for listener mode. Pipes and files keep their existing
  behavior. No kernel or socket ABI change.
- No transport, socket, scheduler, signal, lock, timer, DMA, or driver
  bug was observed. All four cases are correct on the wire.

## Regressions

Step 3 client suite, Step 4 server suite, Step 5 matrix — all pass after the
`nc -l` fix. No kernel changes in Step 6.
