"""Bounded synthetic routers. No audit code is imported here."""
import json
import socket
import struct

MAC = bytes.fromhex("525400123456")
PEER = bytes.fromhex("020304050607")
GUEST = bytes([10, 0, 2, 15])
GATEWAY = bytes([10, 0, 2, 2])
DEST = bytes([192, 0, 2, 9])


def checksum(raw):
    if len(raw) & 1:
        raw += b"\0"
    total = sum(struct.unpack("!" + "H" * (len(raw)//2), raw))
    while total >> 16:
        total = (total & 65535) + (total >> 16)
    return (~total) & 65535


def ip_frame(source, body):
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20+len(body), 0, 0x4000, 64, 1, 0, source, GUEST)
    ip = ip[:10] + struct.pack("!H", checksum(ip)) + ip[12:]
    raw = MAC + PEER + b"\x08\x00" + ip + body
    return raw + b"\0" * max(0, 60-len(raw))


def error(original, kind=11, code=0):
    quote = original[14:42]  # IPv4 header + first eight ICMP bytes only.
    body = struct.pack("!BBHI", kind, code, 0, 0) + quote
    return body[:2] + struct.pack("!H", checksum(body)) + body[4:]


class Peer:
    def __init__(self, sock, target, injections):
        self.sock, self.target, self.injections = sock, target, injections
        self.profile = "clean"
        self.traces = []
        self.pings = 0
        sock.setblocking(False)

    def inject(self, raw, reason):
        self.injections.write(json.dumps(dict(hex=raw.hex(), reason=reason))+"\n")
        self.injections.flush()
        self.sock.sendto(raw, self.target)

    def poll(self):
        for _ in range(64):
            try:
                raw = self.sock.recv(2048)
            except BlockingIOError:
                return
            assert len(raw) <= 1514
            if raw[12:14] == b"\x08\x06":
                if len(raw)>=42 and raw[20:22]==b"\0\1":
                    body = struct.pack("!HHBBH", 1, 0x800, 6, 4, 2) + PEER + raw[38:42] + MAC + GUEST
                    self.inject(MAC+PEER+b"\x08\x06"+body+b"\0"*18, "arp")
                continue
            if len(raw)<42 or raw[12:14]!=b"\x08\0" or raw[23]!=1:
                continue
            body=raw[34:14+int.from_bytes(raw[16:18],"big")]
            if body[0]!=8:
                continue
            trace=raw[18:20]==b"\0\1"
            if trace:
                self.traces.append(dict(ttl=raw[22], identity=body[4:8].hex(), raw=raw.hex()))
                assert len(self.traces)<=1000
            else:
                self.pings+=1
            if self.profile=="drop":
                continue
            if not trace or raw[22]>=3:
                response=bytes([0,0,0,0])+body[4:]
                response=response[:2]+struct.pack("!H",checksum(response))+response[4:]
                self.inject(ip_frame(raw[30:34],response),"echo")
                continue
            if self.profile=="forged":
                bad=bytearray(error(raw)); bad[34]^=1
                bad[2:4]=b"\0\0"; bad[2:4]=struct.pack("!H",checksum(bad))
                self.inject(ip_frame(bytes([192,0,2,66]),bad),"wrong-sequence")
                bad=bytearray(error(raw)); bad[2]^=1
                self.inject(ip_frame(bytes([192,0,2,66]),bad),"bad-checksum")
            source=GATEWAY if raw[22]==1 else bytes([10,0,2,3])
            kind,code=(3,1) if self.profile=="unreachable" else (11,0)
            self.inject(ip_frame(source,error(raw,kind,code)),"unreachable" if kind==3 else "hop")
