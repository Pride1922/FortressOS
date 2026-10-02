"""Bounded finite Ethernet TCP test peer. Does not import the wire auditor."""
import hashlib
import json
import socket
import struct
import time

GUEST_MAC = bytes.fromhex('525400123456')
PEER_MAC = bytes.fromhex('020304050607')
GUEST_IP = bytes([10, 0, 2, 15])
PEER_IP = bytes([10, 0, 2, 2])


def checksum(data):
    padded = data + b'\0' * (len(data) & 1)
    value = sum(struct.unpack('!%dH' % (len(padded) // 2), padded))
    while value >> 16:
        value = (value & 65535) + (value >> 16)
    return (~value) & 65535


def frame(source, dest, seq, ack, flags, data=b'', window=8192, mss=None, ident=0x1234):
    option = b'' if mss is None else struct.pack('!BBH', 2, 4, mss)
    tcp = struct.pack('!HHIIBBHHH', source, dest, seq & 0xffffffff, ack & 0xffffffff,
                      (5 + len(option) // 4) << 4, flags, window, 0, 0) + option + data
    pseudo = PEER_IP + GUEST_IP + struct.pack('!BBH', 0, 6, len(tcp))
    tcp = tcp[:16] + struct.pack('!H', checksum(pseudo + tcp)) + tcp[18:]
    ip = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(tcp), ident, 0x4000,
                     64, 6, 0, PEER_IP, GUEST_IP)
    ip = ip[:10] + struct.pack('!H', checksum(ip)) + ip[12:]
    raw = GUEST_MAC + PEER_MAC + b'\x08\x00' + ip + tcp
    return raw + bytes(max(0, 60 - len(raw)))


def decode(raw):
    if len(raw) < 54 or raw[12:14] != b'\x08\x00' or raw[23] != 6:
        return None
    assert raw[14] == 0x45 and not (int.from_bytes(raw[20:22], 'big') & 0x3fff)
    length = int.from_bytes(raw[16:18], 'big')
    assert 40 <= length <= 1500 and 14 + length <= len(raw)
    ip, tcp = raw[14:34], raw[34:14 + length]
    assert checksum(ip) == 0
    assert checksum(ip[12:20] + struct.pack('!BBH', 0, 6, len(tcp)) + tcp) == 0
    header = (tcp[12] >> 4) * 4
    assert 20 <= header <= len(tcp) and not any(raw[14 + length:])
    mss = None
    cursor = 20
    while cursor < header:
        kind = tcp[cursor]
        if kind == 0:
            assert not any(tcp[cursor:header]); break
        if kind == 1: cursor += 1; continue
        assert cursor + 2 <= header
        size = tcp[cursor + 1]
        assert size >= 2 and cursor + size <= header
        assert kind == 2 and size == 4 and mss is None, 'unsupported negotiation'
        mss = int.from_bytes(tcp[cursor + 2:cursor + 4], 'big'); cursor += size
    source, dest, seq, ack = struct.unpack_from('!HHII', tcp)
    return dict(source=source, dest=dest, seq=seq, ack=ack, flags=tcp[13],
                window=int.from_bytes(tcp[14:16], 'big'), data=tcp[header:],
                ip_source=ip[12:16], ip_dest=ip[16:20], mss=mss)


def offset(sequence, base):
    result = (sequence - base) & 0xffffffff
    assert result != 0x80000000, 'ambiguous half-range sequence'
    return result if result < 0x80000000 else result - 0x100000000


class Connection:
    def __init__(self, peer, port, expected, response, active=False, guest_port=9000,
                 profile='clean', mss=536, hold=False):
        assert len(expected) <= 65536 and len(response) <= 65536
        self.peer, self.port, self.guest_port = peer, port, guest_port
        self.expected, self.response = expected, response
        self.active, self.profile, self.mss, self.hold = active, profile, mss, hold
        self.iss = (0xfffffff0 + len(peer.connections) * 100000) & 0xffffffff
        self.irs = None
        self.next = 0
        self.una = 0
        self.rx = 1
        self.ooo = {}
        self.consumed = bytearray()
        self.retained = []
        self.window = 8192
        self.remote_mss = 536
        self.started = time.monotonic()
        self.state = 'SYN' if active else 'LISTEN'
        self.remote_fin = self.own_fin = self.done = False
        self.fin_at = None
        self.faults = set()
        self.guest_fin_seen = 0
        self.retry_count = 0
        if active:
            self.send(2 | (0xc0 if profile == 'ecn' else 0), retain=True)

    def wire(self, flags, data=b'', at=None, ack=None):
        return frame(self.port, self.guest_port, self.iss + (self.next if at is None else at),
                     0 if self.irs is None else self.irs + (self.rx if ack is None else ack),
                     flags, data, 0 if self.profile == 'zero-window' and 'opened' not in self.faults else 8192,
                     self.mss if flags & 2 else None)

    def send(self, flags, data=b'', retain=False):
        at = self.next
        size = len(data) + bool(flags & 2) + bool(flags & 1)
        if retain:
            assert len(self.retained) < 32 and sum(len(r['data']) for r in self.retained) + len(data) <= 8192
            self.retained.append(dict(at=at, flags=flags, data=data, end=at + size,
                                      deadline=time.monotonic() + .8, retries=0))
            self.next += size
        raw = self.wire(flags, data, at)
        fault = None
        if self.profile == 'handshake-loss' and flags & 2 and 'syn' not in self.faults:
            self.faults.add('syn'); fault = 'drop-syn'
        if self.profile == 'fin-loss' and flags & 1 and 'fin' not in self.faults:
            self.faults.add('fin'); fault = 'drop-fin'
        if fault:
            self.peer.event('fault', port=self.port, fault=fault)
        else:
            self.peer.inject(raw)

    def input(self, packet):
        flags = packet['flags']
        if flags & 4:
            if self.hold:
                self.state = 'EXPIRED'; self.faults.add('expiry-reset')
                self.peer.event('half-open-reset', port=self.port)
                return
            raise AssertionError('unexpected peer reset on port %d' % self.port)
        if flags & 2:
            if self.irs is None:
                self.irs = packet['seq']; self.window = packet['window']
                self.remote_mss = packet.get('mss') or 536
                assert 1 <= self.remote_mss <= 1460
                if self.active:
                    assert flags & 16 and packet['ack'] == (self.iss + 1) & 0xffffffff
                    assert not flags & 0xc0, 'ECN unexpectedly negotiated'
                    self.una = 1; self.retained.clear(); self.state = 'ESTABLISHED'
                    if not self.hold:
                        if self.profile == 'ecn':
                            self.faults.add('final-ack'); self.peer.event('fault', port=self.port, fault='drop-final-handshake-ack')
                            self.state = 'ACK_DELAYED'
                        else: self.send(16)
                else:
                    self.state = 'SYN_RCVD'; self.send(18, retain=True)
            else:
                assert packet['seq'] == self.irs
                if self.active:
                    if not self.hold:
                        self.send(16)
                        if self.state == 'ACK_DELAYED': self.state = 'ESTABLISHED'
                else:
                    self.peer.inject(self.wire(18, at=0))
            return
        if self.irs is None: return
        if flags & 16:
            ack = offset(packet['ack'], self.iss)
            assert ack <= self.next, 'ACK beyond submitted sequence space'
            if ack > self.una:
                self.una = ack
                fresh = []
                for record in self.retained:
                    if record['end'] <= ack: continue
                    if record['at'] < ack:
                        trim = ack - record['at']
                        assert not record['flags'] & 3 and trim <= len(record['data'])
                        record['data'] = record['data'][trim:]; record['at'] = ack
                    fresh.append(record)
                self.retained = fresh
            self.window = packet['window']
            if self.state == 'SYN_RCVD' and self.una >= 1: self.state = 'ESTABLISHED'
        data = packet['data']
        at = offset(packet['seq'], self.irs)
        if data:
            assert at >= 1 and at + len(data) <= len(self.expected) + 1
            assert data == self.expected[at - 1:at - 1 + len(data)], 'wrong application bytes'
            if self.profile == 'loss-reorder' and 'rx-loss' not in self.faults:
                self.faults.add('rx-loss'); self.peer.event('fault', port=self.port, fault='drop-guest-data'); return
            if self.profile == 'zero-window' and 'opened' not in self.faults:
                assert len(data) == 1, 'unbounded zero-window probe'
                self.faults.add('opened'); self.peer.event('probe', port=self.port)
            for index, byte in enumerate(data, at):
                if index < self.rx: continue
                assert index - self.rx < 8192 and len(self.ooo) < 8192
                assert self.ooo.setdefault(index, byte) == byte
            while self.rx in self.ooo:
                self.consumed.append(self.ooo.pop(self.rx)); self.rx += 1
        if flags & 1:
            self.guest_fin_seen += 1
            end = at + len(data)
            assert end == len(self.expected) + 1
            self.fin_at = end
        if self.fin_at == self.rx:
            self.remote_fin = True; self.rx += 1
        if data or flags & 1:
            if self.profile == 'fin-loss' and flags & 1 and self.guest_fin_seen == 1:
                self.peer.event('fault', port=self.port, fault='drop-fin-ack'); return
            self.send(16)
        if self.own_fin and self.una == self.next and self.remote_fin:
            self.done = True; self.state = 'DONE'
        self.pump()

    def pump(self):
        now = time.monotonic()
        assert self.done or now - self.started < 120, 'finite peer deadline exceeded'
        if self.done or self.hold: return
        for record in self.retained:
            if now >= record['deadline']:
                assert record['retries'] < 8, 'retry budget exhausted'
                record['retries'] += 1; self.retry_count += 1
                record['deadline'] = now + min(2, .8 * 2 ** record['retries'])
                self.peer.event('retry', port=self.port, at=record['at'], flags=record['flags'])
                self.peer.inject(self.wire(record['flags'], record['data'], record['at']))
        if self.state != 'ESTABLISHED': return
        ready = (self.active and self.profile != 'serial') or self.remote_fin
        if self.profile == 'fin-loss' and not self.active and self.guest_fin_seen < 2: ready = False
        if not ready: return
        for _ in range(4):
            position = self.next - 1
            room = min(self.window - (self.next - self.una), 8192 - (self.next - self.una))
            size = min(self.mss or 536, self.remote_mss, len(self.response) - position, room)
            if size <= 0: break
            if self.profile == 'loss-reorder' and 'tx-order' not in self.faults and size * 2 <= len(self.response) - position:
                self.faults.add('tx-order')
                # Retain both; inject the second twice, leaving the first gap for its timer.
                self.send(24, self.response[position:position + size], True)
                first = self.retained[-1]
                # The first was injected above. To actually reorder, this profile's
                # injection wrapper drops its first data frame (see Peer.inject).
                self.send(24, self.response[position + size:position + size * 2], True)
                self.peer.inject(self.wire(24, self.response[position + size:position + size * 2], first['at'] + size))
                self.peer.event('fault', port=self.port, fault='duplicate-and-gap')
                continue
            self.send(24, self.response[position:position + size], True)
        if self.next == len(self.response) + 1 and self.una == self.next and not self.own_fin and self.window > 0:
            self.own_fin = True; self.send(17, retain=True)
        if self.own_fin and self.una == self.next and self.remote_fin:
            self.done = True; self.state = 'DONE'

    def summary(self):
        return dict(port=self.port, guest_port=self.guest_port, active=self.active, profile=self.profile,
                    iss=self.iss, irs=self.irs, state=self.state, rx=len(self.consumed),
                    hash=hashlib.sha256(self.consumed).hexdigest(), retries=self.retry_count,
                    faults=sorted(self.faults), expected=self.expected.hex(), response=self.response.hex(),
                    half_open=self.hold, allow_restart='restart' in self.faults, mss=self.mss)


class Peer:
    def __init__(self, sock, target, injections, events):
        self.sock, self.target = sock, target
        self.injections, self.events = injections, events
        self.connections = []
        self.listeners = {}
        self.packets = self.event_count = 0
        self.drop_data_ports = set()
        self.syn_seen = {}
        sock.setblocking(False)

    def event(self, name, **values):
        self.event_count += 1
        assert self.event_count <= 20000, 'event budget exhausted'
        self.events.write(json.dumps(dict(event=name, time_ns=time.time_ns(), **values)) + '\n')
        self.events.flush()

    def inject(self, raw, invalid=None):
        self.packets += 1
        assert self.packets <= 20000, 'packet budget exhausted'
        packet = decode(raw) if invalid != 'checksum' else None
        if packet and packet['data']:
            conn = next((c for c in self.connections if c.port == packet['source']), None)
            if conn and conn.profile == 'loss-reorder' and conn.port not in self.drop_data_ports:
                self.drop_data_ports.add(conn.port)
                self.event('fault', port=conn.port, fault='drop-peer-first-data'); return
        self.injections.write(json.dumps(dict(time_ns=time.time_ns(), hex=raw.hex(), invalid=invalid)) + '\n')
        self.injections.flush()
        self.sock.sendto(raw, self.target)

    def listen(self, port, expected, response, profile='clean', mss=536):
        self.listeners[port] = (expected, response, profile, mss)

    def active(self, port, expected, response, guest_port=9000, profile='clean', mss=536, hold=False):
        assert len(self.connections) < 8, 'connection record budget exhausted'
        conn = Connection(self, port, expected, response, True, guest_port, profile, mss, hold)
        self.connections.append(conn)
        return conn

    def poll(self):
        for _ in range(64):
            try: raw = self.sock.recv(2048)
            except BlockingIOError: break
            self.packets += 1; assert self.packets <= 20000
            if len(raw) >= 42 and raw[12:14] == b'\x08\x06':
                arp = raw[14:42]
                if arp[:8] == bytes.fromhex('0001080006040001') and arp[24:28] == PEER_IP:
                    assert arp[8:14] == GUEST_MAC and arp[14:18] == GUEST_IP
                    answer = GUEST_MAC + PEER_MAC + b'\x08\x06' + bytes.fromhex('0001080006040002')
                    answer += PEER_MAC + PEER_IP + GUEST_MAC + GUEST_IP
                    self.inject(answer + bytes(60 - len(answer)))
                continue
            packet = decode(raw)
            if packet is None: continue
            assert raw[6:12] == GUEST_MAC and packet['ip_source'] == GUEST_IP and packet['ip_dest'] == PEER_IP
            conn = next((c for c in self.connections if c.port == packet['dest'] and c.guest_port == packet['source']), None)
            if packet['flags'] & 2:
                assert len(self.syn_seen) < 16 or packet['dest'] in self.syn_seen
                self.syn_seen[packet['dest']] = packet['seq']
            if conn is None and packet['flags'] & 2 and packet['dest'] in self.listeners:
                assert len(self.connections) < 8, 'connection record budget exhausted'
                expected, response, profile, mss = self.listeners[packet['dest']]
                conn = Connection(self, packet['dest'], expected, response, guest_port=packet['source'], profile=profile, mss=mss)
                self.connections.append(conn)
            if conn is not None:
                conn.input(packet)
                if conn.profile == 'malformed' and conn.state == 'ESTABLISHED' and 'invalid' not in conn.faults:
                    conn.faults.add('invalid')
                    bad = bytearray(conn.wire(24, b'BAD')); bad[50] ^= 1
                    self.inject(bytes(bad), 'checksum')
                    self.inject(frame(conn.port + 1, conn.guest_port, conn.iss + 1, conn.irs + conn.rx, 24, b'BAD'), 'tuple')
                    self.inject(conn.wire(16, ack=100000), 'future-ack')
        for conn in self.connections: conn.pump()

    def summaries(self): return [c.summary() for c in self.connections]
