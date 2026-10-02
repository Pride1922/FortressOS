"""Host-only independent fixture gates; no kernel/scheduler acceptance claim."""
import io
import json
import struct
import sys
import tempfile
import time
import socket
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import net_tcp_socket_peer as peer
import net_tcp_wire_audit as audit


class Fake:
    def __init__(self): self.connections = []; self.frames = []
    def inject(self, raw): self.frames.append(raw)
    def event(self, *args, **kwargs): pass


class FixtureTests(unittest.TestCase):
    def test_literal_vectors(self):
        vectors = json.loads((Path(__file__).parent / 'fixtures/net_tcp_wire_vectors.json').read_text())
        for v in vectors:
            raw = bytes.fromhex(v['hex'])
            self.assertEqual(peer.frame(7777, 40000, v['sequence'], v['ack'], v['flags'], bytes.fromhex(v['payload']), mss=v['mss']), raw)
            for decode in (peer.decode, audit.parse):
                result = decode(raw)
                for key, expected in [('seq', v['sequence']), ('ack', v['ack']), ('flags', v['flags']), ('data', bytes.fromhex(v['payload']))]:
                    self.assertEqual(result[key], expected)
            for decode in (peer.decode, audit.parse): self.assertEqual(decode(raw)['mss'], v['mss'])
            for index in (24, 50, 46):
                bad = bytearray(raw); bad[index] ^= 1
                with self.assertRaises(AssertionError): audit.parse(bad)
            # Invalid data offset with a recomputed, valid TCP checksum must
            # fail header bounds rather than merely checksum validation.
            bad = bytearray(raw); bad[46] = 0xf0; bad[50:52] = b'\0\0'
            end = 14 + int.from_bytes(bad[16:18], 'big')
            segment = bytes(bad[34:end])
            bad[50:52] = peer.checksum(bytes(bad[26:34]) + b'\0\x06' + len(segment).to_bytes(2, 'big') + segment).to_bytes(2, 'big')
            with self.assertRaises(AssertionError): audit.parse(bad)

    def test_wrap_duplicate_gap_and_ack_bounds(self):
        f = Fake(); c = peer.Connection(f, 7777, b'abcdef', b'abcdef')
        f.connections.append(c)
        syn = dict(seq=0xfffffffe, ack=0, flags=2, window=8192, data=b'')
        c.input(syn)
        base = dict(seq=0xffffffff, ack=(c.iss + 1) & 0xffffffff, flags=16, window=8192, data=b'')
        c.input(base)
        c.input(dict(base, seq=2, data=b'def'))
        self.assertEqual(c.rx, 1)
        c.input(dict(base, data=b'abc'))
        c.input(dict(base, data=b'abc'))
        self.assertEqual(bytes(c.consumed), b'abcdef')
        self.assertEqual(c.rx, 7)
        with self.assertRaises(AssertionError): c.input(dict(base, ack=(c.iss + 100) & 0xffffffff))
        with self.assertRaises(AssertionError): c.input(dict(base, data=b'xxx'))
        with self.assertRaises(AssertionError): peer.offset(0x80000000, 0)

    def test_resources_and_deadline(self):
        f = Fake()
        with self.assertRaises(AssertionError): peer.Connection(f, 1, bytes(65537), b'')
        c = peer.Connection(f, 1, bytes(65536), b'')
        c.irs = 0; c.state = 'ESTABLISHED'; c.started = time.monotonic() - 121
        with self.assertRaises(AssertionError): c.pump()
        c.started = time.monotonic()
        with self.assertRaises(AssertionError):
            c.input(dict(seq=9000, ack=0, flags=16, window=8192, data=b'\0'))
        for _ in range(32): c.send(24, b'x', True)
        with self.assertRaises(AssertionError): c.send(24, b'x', True)
        c.retained[0]['retries'] = 8; c.retained[0]['deadline'] = 0
        with self.assertRaises(AssertionError): c.pump()

    def test_zero_window_blocks_fin_and_reopens(self):
        f = Fake(); c = peer.Connection(f, 1, b'', b'', active=True)
        c.irs = 10; c.state = 'ESTABLISHED'; c.una = 1; c.retained.clear(); c.window = 0
        c.pump(); self.assertFalse(c.own_fin)
        c.window = 1; c.pump(); self.assertTrue(c.own_fin)
        self.assertEqual(peer.decode(f.frames[-1])['flags'], 17)

    def test_peer_record_packet_event_limits(self):
        class Socket:
            def setblocking(self, value): pass
            def sendto(self, data, address): pass
        p = peer.Peer(Socket(), ('127.0.0.1', 1), io.StringIO(), io.StringIO())
        for i in range(8): p.active(40000 + i, b'', b'', hold=True)
        with self.assertRaises(AssertionError): p.active(40008, b'', b'', hold=True)
        p.event_count = 20000
        with self.assertRaises(AssertionError): p.event('overflow')
        p.packets = 20000
        with self.assertRaises(AssertionError): p.inject(peer.frame(1, 2, 1, 0, 2))

    def test_no_shared_protocol_imports(self):
        import ast
        for module, forbidden in ((peer, 'net_tcp_wire_audit'), (audit, 'net_tcp_socket_peer')):
            tree = ast.parse(Path(module.__file__).read_text())
            for node in ast.walk(tree):
                if isinstance(node, ast.Import): self.assertNotIn(forbidden, [n.name for n in node.names])
                if isinstance(node, ast.ImportFrom): self.assertNotEqual(node.module, forbidden)

    def test_uart_drain_without_logger_progress(self):
        from test_net_tcp_matrix import Case
        with tempfile.TemporaryDirectory() as folder:
            case = Case(Path(folder) / 'case', 'bios', 'e1000', 'socket')
            producer, case.uart = socket.socketpair()
            producer.settimeout(.5); case.uart.settimeout(.05); case.start_reader()
            body = b'x' * 500000  # Exceeds socket buffering; no log writes yet.
            producer.sendall(body)
            end = time.monotonic() + 2
            while len(case.serial_buffer) < len(body) and time.monotonic() < end: time.sleep(.005)
            self.assertEqual(bytes(case.serial_buffer), body)
            self.assertEqual(case.log.stat().st_size, 0)
            producer.close(); case.close()
            self.assertEqual(case.log.read_bytes(), body)
            self.assertFalse(case.reader.is_alive())

    def test_audit_requires_complete_injection_input(self):
        with tempfile.TemporaryDirectory() as folder:
            pcap = Path(folder) / 'wire.pcap'
            raw = peer.frame(7777, 40000, 1, 0, 2)
            pcap.write_bytes(struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1)
                            + struct.pack('<IIII', 1, 0, len(raw), len(raw)) + raw)
            log = Path(folder) / 'injection.jsonl'
            with self.assertRaises(FileNotFoundError): audit.audit(pcap, log, [])
            for body, error in (('{broken', json.JSONDecodeError), ('{}', KeyError),
                                ('{"hex":"not-hex"}', ValueError), ('', AssertionError)):
                with self.subTest(body=body):
                    log.write_text(body)
                    with self.assertRaises(error): audit.audit(pcap, log, [])

    def test_audit_rejects_faked_pass_and_truncation(self):
        # A forged PASS does not bypass parsing/checksum failure.
        raw = bytearray(peer.frame(7777, 40000, 1, 0, 2)); raw[6:12] = bytes.fromhex('525400123456')
        raw[24] ^= 1
        with tempfile.TemporaryDirectory() as folder:
            pcap = Path(folder) / 'bad.pcap'
            pcap.write_bytes(struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1) + struct.pack('<IIII', 1, 0, len(raw), len(raw)) + raw)
            with self.assertRaises(AssertionError): audit.audit(pcap, None, [dict(port=40000, guest_port=7777, expected='', response='', status='PASS')])
            pcap.write_bytes(pcap.read_bytes()[:-1])
            with self.assertRaises(AssertionError): list(audit.records(pcap))


if __name__ == '__main__': unittest.main()
