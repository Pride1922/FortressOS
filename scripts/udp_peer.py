#!/usr/bin/env python3
"""Finite IPv4 UDP peer for Windows/Linux physical Phase 5 acceptance."""
import argparse
import socket


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='mode', required=True)
    server = sub.add_parser('echo')
    server.add_argument('--bind', required=True)
    server.add_argument('--port', type=int, default=7777)
    server.add_argument('--count', type=int, default=4)
    client = sub.add_parser('client')
    client.add_argument('ip')
    client.add_argument('--port', type=int, default=7777)
    client.add_argument('--count', type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or not 1 <= args.count <= 100:
        parser.error('port 1..65535; count 1..100')
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
        peer.settimeout(30 if args.mode == 'echo' else 5)
        if args.mode == 'echo':
            peer.bind((args.bind, args.port))
            print(f'UDP echo listening on {args.bind}:{args.port}, {args.count} datagrams, 30-second per-receive timeout', flush=True)
            for _ in range(args.count):
                data, source = peer.recvfrom(65536)
                if len(data) > 1472:
                    raise RuntimeError('oversized Phase 5 datagram')
                peer.sendto(data, source)
                print(f'echoed {len(data)} bytes to {source}: {data!r}', flush=True)
        else:
            peer.bind(('0.0.0.0', 0))
            dest = (socket.gethostbyname(args.ip), args.port)
            for i in range(args.count):
                data = f'fortress-phase5-{i + 1}'.encode()
                peer.sendto(data, dest)
                returned, source = peer.recvfrom(65536)
                if returned != data or source != dest:
                    raise RuntimeError(f'echo mismatch: {source} {returned!r}')
                print(f'PASS {i + 1}: {len(data)} exact bytes from {source}', flush=True)
    print('UDP peer PASS', flush=True)


if __name__ == '__main__':
    main()
