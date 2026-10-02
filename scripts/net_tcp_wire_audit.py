"""Independent wire gate: no imports from the synthetic peer or kernel codecs."""
import hashlib
import json
from pathlib import Path


def folded(data):
    total = 0
    for i in range(0, len(data), 2):
        total += data[i] * 256 + (data[i + 1] if i + 1 < len(data) else 0)
        total = (total & 65535) + (total >> 16)
    return (total & 65535) + (total >> 16)


def parse(raw):
    assert 60 <= len(raw) <= 1514, 'Ethernet bounds'
    if raw[12:14] != b'\x08\x00': return None
    assert raw[14] >> 4 == 4
    ihl = (raw[14] & 15) * 4
    length = int.from_bytes(raw[16:18], 'big')
    assert 20 <= ihl <= 60 and ihl <= length <= 1500 and 14 + length <= len(raw)
    assert folded(raw[14:14 + ihl]) == 65535, 'IPv4 checksum'
    assert not any(raw[14 + length:]), 'nonzero Ethernet padding'
    if raw[23] != 6: return None
    assert not (int.from_bytes(raw[20:22], 'big') & 0x3fff), 'fragmented TCP'
    segment = raw[14 + ihl:14 + length]
    assert len(segment) >= 20
    pseudo = raw[26:34] + b'\0\x06' + len(segment).to_bytes(2, 'big')
    assert folded(pseudo + segment) == 65535, 'TCP checksum'
    header = (segment[12] >> 4) * 4
    assert 20 <= header <= 60 and header <= len(segment)
    assert segment[12] & 15 == 0 and not segment[13] & 0x20, 'reserved/urgent TCP'
    mss = None
    position = 20
    while position < header:
        kind = segment[position]
        if kind == 0:
            assert not any(segment[position:header]); break
        if kind == 1: position += 1; continue
        assert position + 2 <= header
        size = segment[position + 1]
        assert size >= 2 and position + size <= header
        if kind == 2:
            assert size == 4 and mss is None
            mss = int.from_bytes(segment[position + 2:position + 4], 'big')
        else: raise AssertionError('unnegotiated TCP option')
        position += size
    return dict(source=int.from_bytes(segment[:2], 'big'), dest=int.from_bytes(segment[2:4], 'big'),
                seq=int.from_bytes(segment[4:8], 'big'), ack=int.from_bytes(segment[8:12], 'big'),
                flags=segment[13], window=int.from_bytes(segment[14:16], 'big'),
                data=segment[header:], mss=mss, ips=raw[26:30], ipd=raw[30:34])


def records(path):
    """Classic pcap container with timestamps; deliberately strict on truncation."""
    assert Path(path).stat().st_size <= 24 + 20000 * (16 + 1514), 'pcap resource bound'
    raw = Path(path).read_bytes()
    assert len(raw) >= 24, 'missing pcap header'
    magic = raw[:4]
    assert magic in (b'\xd4\xc3\xb2\xa1', b'\xa1\xb2\xc3\xd4', b'\x4d\x3c\xb2\xa1', b'\xa1\xb2\x3c\x4d')
    order = 'little' if magic[0] in (0xd4, 0x4d) else 'big'
    scale = 1 if magic in (b'\x4d\x3c\xb2\xa1', b'\xa1\xb2\x3c\x4d') else 1000
    assert int.from_bytes(raw[20:24], order) == 1
    at = 24
    while at < len(raw):
        assert at + 16 <= len(raw), 'truncated pcap record header'
        seconds, fraction, size, original = [int.from_bytes(raw[at + i:at + i + 4], order) for i in (0, 4, 8, 12)]
        at += 16
        assert size == original and 0 < size <= 1514 and at + size <= len(raw), 'truncated pcap frame'
        yield seconds * 1000000000 + fraction * scale, raw[at:at + size]
        at += size


def audit(pcap, injection_path, scenarios):
    """Scenarios specify expected bytes; PASS/status fields are intentionally ignored."""
    timeline = []
    injections = {}
    if injection_path is not None:
        assert Path(injection_path).stat().st_size <= 20000 * 3200, 'injection resource bound'
        for count, line in enumerate(Path(injection_path).read_text().splitlines()):
            assert count < 20000
            row = json.loads(line)
            raw = bytes.fromhex(row['hex'])
            entry = injections.setdefault(raw, dict(count=0, invalid=row.get('invalid')))
            assert entry['invalid'] == row.get('invalid')
            entry['count'] += 1
    # filter-dump records both directions in one clock domain. QEMU's pcap
    # clock need not equal Python wall time (observed one-hour WSL offset).
    # Never merge those clocks. Match inbound bytes to injection records, then
    # use capture order to prove ACKs refer to already submitted sequence space.
    for stamp, raw in records(pcap):
        assert len(timeline) < 20000, 'capture packet budget'
        direction = int(raw[6:12] == bytes.fromhex('525400123456'))
        invalid = None
        if not direction and injection_path is not None:
            assert raw in injections and injections[raw]['count'] > 0, 'unlogged inbound frame'
            injections[raw]['count'] -= 1; invalid = injections[raw]['invalid']
        timeline.append((stamp, direction, raw, invalid))
    specs = {(s['guest_port'], s['port']): s for s in scenarios}
    assert len(specs) <= 8
    states = {}
    for stamp, direction, raw, invalid in timeline:
        if invalid == 'checksum':
            try: parse(raw)
            except AssertionError: continue
            raise AssertionError('declared checksum fault is valid')
        packet = parse(raw)
        if packet is None: continue
        if not direction and invalid == 'overflow':
            assert packet['flags'] == 2 and not packet['data']; continue
        if direction == 0:
            if injection_path is not None:
                assert raw[:12] == bytes.fromhex('525400123456020304050607')
            key = (packet['dest'], packet['source'])
            if invalid == 'tuple':
                assert key not in specs; continue
        else:
            assert raw[6:12] == bytes.fromhex('525400123456')
            key = (packet['source'], packet['dest'])
        if key not in specs:
            # Refusals of intentionally invalid tuples are admissible, data is not.
            assert not packet['data'] and packet['flags'] & 4, ('unknown TCP tuple', key)
            continue
        spec = specs[key]
        assert (packet['ips'], packet['ipd']) == ((bytes([10, 0, 2, 15]), bytes([10, 0, 2, 2])) if direction else
                                                (bytes([10, 0, 2, 2]), bytes([10, 0, 2, 15])))
        state = states.setdefault(key, dict(gisn=None, pisn=None, peer_end=0, guest_end=0,
                                            data={}, peer_data={}, peer_contiguous=1, peer_fin=None,
                                            fin=False, handshake=False, acks=0, peer_syn_time=None, expired=False))
        base_key = 'gisn' if direction else 'pisn'
        if packet['flags'] & 2:
            if direction and state[base_key] is not None and state[base_key] != packet['seq'] and spec.get('allow_restart'):
                state.update(gisn=None, peer_end=1, guest_end=0, data={}, peer_data={}, peer_contiguous=1,
                             peer_fin=None, fin=False, handshake=False, acks=0)
            if state[base_key] is None: state[base_key] = packet['seq']
            assert state[base_key] == packet['seq'], 'changed ISN on retransmission'
            if direction: state['guest_end'] = max(state['guest_end'], 1)
            else:
                state['peer_end'] = max(state['peer_end'], 1)
                if state['peer_syn_time'] is None: state['peer_syn_time'] = stamp
        if state[base_key] is None:
            raise AssertionError('segment before SYN')
        relative = (packet['seq'] - state[base_key]) & 0xffffffff
        payload = bytes.fromhex(spec['expected'] if direction else spec['response'])
        if invalid == 'future-ack':
            assert not packet['data']; continue
        if packet['data']:
            assert 1 <= relative and relative - 1 + len(packet['data']) <= len(payload), 'stream bounds'
            assert packet['data'] == payload[relative - 1:relative - 1 + len(packet['data'])], 'stream bytes'
            if direction:
                if 'mss' in spec: assert len(packet['data']) <= (spec['mss'] or 536), 'MSS exceeded'
                for index, byte in enumerate(packet['data'], relative - 1):
                    assert state['data'].setdefault(index, byte) == byte, 'conflicting retransmission'
            else:
                for index, byte in enumerate(packet['data'], relative):
                    assert state['peer_data'].setdefault(index, byte) == byte, 'conflicting peer retransmission'
                while state['peer_contiguous'] in state['peer_data']: state['peer_contiguous'] += 1
        end = relative + len(packet['data']) + bool(packet['flags'] & 3)
        state['guest_end' if direction else 'peer_end'] = max(state['guest_end' if direction else 'peer_end'], end)
        if packet['flags'] & 16:
            other_base = state['pisn' if direction else 'gisn']
            assert other_base is not None, 'ACK before opposite SYN'
            acknowledged = (packet['ack'] - other_base) & 0xffffffff
            assert acknowledged <= state['peer_end' if direction else 'guest_end'], 'ACK of unsubmitted bytes'
            if direction:
                contiguous = state['peer_contiguous'] + (state['peer_fin'] == state['peer_contiguous'])
                assert acknowledged <= contiguous, 'cumulative ACK crosses an injected gap'
            if acknowledged >= 1: state['handshake'] = True
            if direction: state['acks'] = max(state['acks'], acknowledged)
        if packet['flags'] & 1:
            assert relative + len(packet['data']) == len(payload) + 1, 'FIN sequence'
            if direction: state['fin'] = True
            else: state['peer_fin'] = relative + len(packet['data'])
        if direction and packet['flags'] & 4 and 'expiry-reset' in spec.get('faults', []):
            assert stamp - state['peer_syn_time'] >= 30 * 1000000000, 'early half-open expiry'
            state['expired'] = True
    results = []
    for key, spec in specs.items():
        state = states.get(key)
        if spec.get('half_open'):
            assert state and state['gisn'] is not None and not state['data'] and not state['fin']
            if 'expiry-reset' in spec.get('faults', []): assert state['expired'], 'expiry reset absent from capture'
            continue
        assert state and state['handshake'] and state['fin'], ('missing handshake/FIN', key)
        expected = bytes.fromhex(spec['expected'])
        assert len(state['data']) == len(expected) and bytes(state['data'][i] for i in range(len(expected))) == expected
        assert state['acks'] == len(bytes.fromhex(spec['response'])) + 2, ('missing cumulative data/FIN ACK', key, state['acks'])
        results.append(dict(tuple=list(key), bytes=len(expected), sha256=hashlib.sha256(expected).hexdigest()))
    return results
