#!/usr/bin/env python3
"""Wi-Fi kernel stack-frame gate.

Compiles every Wi-Fi path source with the real kernel CFLAGS plus -fstack-usage and
fails if any function's frame exceeds the budget. Regression guard for the 3,250-byte
wlan_send_packet frames that overflowed the 16 KiB kernel stack.

Usage: check_wifi_stack.py <budget_bytes> <cc> <outdir> <cflags...> -- <sources...>
"""
import os
import subprocess
import sys


# Authorized exceptions (function name -> max frame bytes). Keep this list short and justified.
#  wpa2_sm_rx_eapol: EAPOL M3 path holds a 512 B frame copy + 256 B key-data buffer for MIC
#  verification; measured 1072 B, called from the net worker with >12 KiB of stack headroom.
ALLOWED = {"wpa2_sm_rx_eapol": 1280}


def main() -> int:
    budget = int(sys.argv[1])
    cc = sys.argv[2]
    outdir = sys.argv[3]
    rest = sys.argv[4:]
    sep = rest.index("--")
    cflags, sources = rest[:sep], rest[sep + 1:]
    os.makedirs(outdir, exist_ok=True)

    worst = []
    for src in sources:
        obj = os.path.join(outdir, os.path.basename(src) + ".o")
        subprocess.check_call([cc, *cflags, "-fstack-usage", "-c", src, "-o", obj])
        su = obj[:-2] + ".su"
        if not os.path.exists(su):
            su = os.path.splitext(obj)[0] + ".su"
        with open(su, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                parts = line.rstrip("\n").split("\t")
                if len(parts) < 3:
                    continue
                try:
                    size = int(parts[1])
                except ValueError:
                    continue
                worst.append((size, parts[0], parts[2]))

    worst.sort(reverse=True)
    for size, where, kind in worst[:5]:
        print(f"  {size:6d} B  {where} [{kind}]")
    over = [w for w in worst
            if w[0] > ALLOWED.get(w[1].split(":")[-1], budget)]
    if over:
        for size, where, kind in over:
            print(f"FAIL: {where} uses {size} B (> {ALLOWED.get(where.split(chr(58))[-1], budget)} B limit)")
        return 1
    print(f"PASS: {len(worst)} functions, max {worst[0][0]} B, default budget {budget} B, authorized exceptions {sorted(ALLOWED)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
