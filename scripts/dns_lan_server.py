"""Controlled LAN DNS acceptance peer; run explicitly on the second machine.

Classic IN/A only, fixed test names, finite TCP reads and bounded connections.
This fixture is not an independent capture audit or a general DNS server.
"""
import argparse
import concurrent.futures
import ipaddress
import socket
import struct
import threading


def question(raw):
    if len(raw) < 12:
        raise ValueError('short header')
    ident, flags, qd, an, ns, ar = struct.unpack('!6H', raw[:12])
    if flags != 0x100 or (qd, an, ns, ar) != (1, 0, 0, 0):
        raise ValueError('classic single RD question required')
    at = 12
    labels = []
    while at < len(raw):
        size = raw[at]
        at += 1
        if not size:
            break
        if size > 63 or at + size > len(raw):
            raise ValueError('invalid label')
        labels.append(raw[at:at + size].decode('ascii').lower())
        at += size
    else:
        raise ValueError('unterminated name')
    if at + 4 != len(raw) or raw[at:] != b'\x00\x01\x00\x01':
        raise ValueError('IN/A question required')
    return ident, '.'.join(labels), raw[12:]


def answer(raw, address, tcp=False):
    ident, name, q = question(raw)
    names = {'service.test', 'alias.test', 'missing.test', 'tcp.test', 'stall.test'}
    if name not in names:
        return struct.pack('!6H', ident, 0x8183, 1, 0, 0, 0) + q
    if name == 'missing.test':
        return struct.pack('!6H', ident, 0x8183, 1, 0, 0, 0) + q
    if name in {'tcp.test', 'stall.test'} and not tcp:
        return struct.pack('!6H', ident, 0x8380, 1, 0, 0, 0) + q
    record = b'\xc0\x0c' + struct.pack('!HHIH', 1, 1, 60, 4) + address
    if name == 'alias.test':
        target = b'\x07service\x04test\x00'
        cname = b'\xc0\x0c' + struct.pack('!HHIH', 5, 1, 60, len(target)) + target
        record = cname + target + struct.pack('!HHIH', 1, 1, 60, 4) + address
        count = 2
    else:
        count = 1
    return struct.pack('!6H', ident, 0x8180, 1, count, 0, 0) + q + record


def read_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        part = sock.recv(size - len(data))
        if not part:
            raise ValueError('early EOF')
        data.extend(part)
    return bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bind', required=True, help='peer LAN IPv4 address')
    parser.add_argument('--answer', required=True, help='A record IPv4 address')
    args = parser.parse_args()
    bind = str(ipaddress.IPv4Address(args.bind))
    address = ipaddress.IPv4Address(args.answer).packed
    stop = threading.Event()
    slots = threading.BoundedSemaphore(4)
    def tcp_client(client, peer):
        try:
            with client:
                client.settimeout(40)
                length = struct.unpack('!H', read_exact(client, 2))[0]
                if not 12 <= length <= 4096:
                    raise ValueError('length limit')
                raw = read_exact(client, length)
                _, name, _ = question(raw)
                print('TCP', peer, name, flush=True)
                if name == 'stall.test':
                    stop.wait(40)  # guest must time out on its own before this
                else:
                    response = answer(raw, address, tcp=True)
                    client.sendall(struct.pack('!H', len(response)) + response)
        except (OSError, ValueError) as exc:
            print('TCP rejected:', exc, flush=True)
        finally:
            slots.release()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp, \
         socket.socket(socket.AF_INET, socket.SOCK_STREAM) as tcp, \
         concurrent.futures.ThreadPoolExecutor(4) as pool:
        udp.bind((bind, 53)); udp.settimeout(.1)
        tcp.bind((bind, 53)); tcp.listen(4); tcp.settimeout(.1)
        print(f'DNS peer {bind}:53; A={args.answer}; Ctrl-C stops', flush=True)
        try:
            while True:
                try:
                    raw, peer = udp.recvfrom(4097)
                    if len(raw) > 512:
                        raise ValueError('UDP length limit')
                    _, name, _ = question(raw)
                    print('UDP', peer, name, flush=True)
                    udp.sendto(answer(raw, address), peer)
                except socket.timeout:
                    pass
                except (OSError, ValueError) as exc:
                    print('UDP rejected:', exc, flush=True)
                try:
                    client, peer = tcp.accept()
                    if slots.acquire(blocking=False):
                        pool.submit(tcp_client, client, peer)
                    else:
                        client.close()
                except socket.timeout:
                    pass
        except KeyboardInterrupt:
            stop.set()


if __name__ == '__main__':
    main()
