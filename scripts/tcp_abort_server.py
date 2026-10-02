#!/usr/bin/env python3
"""Case D peer: TCP server that aborts the connection with a real RST.

Accepts one connection, reads a small amount (or nothing), then closes the
socket with SO_LINGER enabled and a zero timeout. On Linux and Windows this
produces a TCP RST rather than a graceful FIN.
"""
import argparse
import socket
import struct
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bind", required=True, help="Local IPv4 to bind")
    ap.add_argument("--port", type=int, required=True, help="Local TCP port")
    ap.add_argument("--log", required=False, help="Log file path")
    ap.add_argument("--linger", type=int, default=0,
                    help="SO_LINGER seconds (0 produces RST)")
    ap.add_argument("--drain", action="store_true",
                    help="Drain bytes before aborting (default: abort immediately)")
    ap.add_argument("--drain-max", type=int, default=65536,
                    help="Maximum bytes to drain if --drain is set")
    args = ap.parse_args()

    logf = open(args.log, "w", encoding="utf-8", newline="\n") if args.log else None

    def log(msg):
        line = f"[{time.strftime('%H:%M:%S')}] {msg}"
        print(line, flush=True)
        if logf:
            logf.write(line + "\n")
            logf.flush()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.bind, args.port))
    srv.listen(1)
    log(f"listening on {args.bind}:{args.port} (abort mode, linger={args.linger})")

    try:
        conn, addr = srv.accept()
        log(f"accept from {addr[0]}:{addr[1]}")
        if args.drain:
            total = 0
            conn.settimeout(2.0)
            try:
                while total < args.drain_max:
                    data = conn.recv(4096)
                    if not data:
                        break
                    total += len(data)
            except socket.timeout:
                log("drain timeout")
            except OSError as e:
                log(f"drain error: {e!r}")
            log(f"drained {total} bytes before abort")
        else:
            # Give the guest a moment to have sent something before we abort.
            time.sleep(0.5)
            log("aborting without reading")
        # SO_LINGER with l_onoff=1, l_linger=0 forces RST on close.
        linger = struct.pack("ii", 1, args.linger)
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, linger)
        conn.close()
        log("closed with SO_LINGER 0 (RST expected on the wire)")
    except KeyboardInterrupt:
        log("interrupted")
    finally:
        srv.close()
        if logf:
            logf.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
