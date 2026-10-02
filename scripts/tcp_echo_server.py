#!/usr/bin/env python3
"""Case A peer: TCP echo server.

Listens on a port, echoes every byte it receives, closes on peer EOF.
Writes a small log to --log if provided.
"""
import argparse
import socket
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bind", required=True, help="Local IPv4 to bind")
    ap.add_argument("--port", type=int, required=True, help="Local TCP port")
    ap.add_argument("--log", required=False, help="Log file path")
    ap.add_argument("--once", action="store_true",
                    help="Accept exactly one connection and exit (default: loop)")
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
    srv.listen(4)
    log(f"listening on {args.bind}:{args.port}")

    try:
        while True:
            conn, addr = srv.accept()
            log(f"accept from {addr[0]}:{addr[1]}")
            total = 0
            try:
                while True:
                    data = conn.recv(4096)
                    if not data:
                        break
                    total += len(data)
                    log(f"recv {len(data)} bytes (total {total}): {data!r}")
                    conn.sendall(data)
                log(f"peer EOF after {total} bytes; closing")
            except OSError as e:
                log(f"connection error: {e!r}")
            finally:
                try:
                    conn.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                conn.close()
            if args.once:
                break
    except KeyboardInterrupt:
        log("interrupted")
    finally:
        srv.close()
        if logf:
            logf.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
