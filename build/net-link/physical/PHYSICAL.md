# Cold cable insertion — physical acceptance

Date: 2026-10-02
Machine: Dell Latitude 5590
I219: 8086:15D7
MAC: c8:f7:50:0e:35:80
Dell IP: 192.168.0.168
Gateway: 192.168.0.1
Boot cmdline: net=192.168.0.168/24,192.168.0.1 verbose

Cases:
1. Boot connected, ping                 PASS
2. Boot disconnected, insert, ping      PASS
3. Boot connected, unplug/replug, ping  PASS
4. Unplug during ping -c 4              PASS (bounded "transmit failed", prompt returned)
5. Boot disconnected, nslookup          PASS (clean EIO, shell usable)
6. Unplug during nc -l, replug, new nc  PASS (old listener failed with error;
                                                new nc -l accepted peer data)

Notes:
- Case 2 answers the PHY question: the I219-LM renegotiates autonomously on
  cable insertion. No MDIC autonegotiation writes were needed.
- Case 6 confirms the documented listener-recovery scope: blocked operations
  fail cleanly; a new listener works after replug; existing connections do
  not resume.
- No [FAIL] or [PANIC] in any case.
- DMA page count and ring addresses stable across flaps (matches the QEMU
  100-flap test with 146 pages).
