#!/usr/bin/env python3
"""Live serial nc boundaries, BIOS/e1000 user/socket; disposable ISO, no disks.

The early-response deadline belongs to the peer, not to nc or the socket ABI.
"""
import concurrent.futures
import json
import socket
import time
import traceback
import uuid
from test_net_tcp_matrix import Case, REPO, save
from net_tcp_wire_audit import records, parse
from net_tcp_socket_peer import Connection, frame


class EarlyPeer(Connection):
    """Advertise zero request space; stream response, then exact-sequence RST."""
    def wire(self, flags, data=b'', at=None, ack=None):
        return frame(self.port, self.guest_port, self.iss + (self.next if at is None else at),
                     0 if self.irs is None else self.irs + self.rx, flags, data, 0,
                     self.mss if flags & 2 else None)

    def input(self, packet):
        if self.done: return
        assert not packet['flags'] & 1, 'request finished despite zero peer window'
        super().input(dict(packet, data=b''))
        if packet['data']:
            assert len(packet['data']) == 1, 'unbounded zero-window probe'
            self.send(16)

    def pump(self):
        if self.done: return
        if self.state == 'ESTABLISHED':
            if not hasattr(self, 'deadline'): self.deadline = time.monotonic() + 3
            if time.monotonic() >= self.deadline:
                assert self.window == 0 and self.una >= 8193, 'response did not saturate guest RX'
                self.peer.inject(self.wire(20, at=self.una))
                self.done = True
                return
        super().pump()


def early_run(root):
    case = Case(root, 'bios', 'e1000', 'socket')
    case.config = dict(profiles=['nc-early-response-zero-window-reset'])
    conn = None
    try:
        case.boot()
        at = case.start('nc -l 9000 < /bin/nc > /dev/null')
        conn = EarlyPeer(case.peer, 40001, b'', b'R' * 32768, active=True)
        case.peer.connections.append(conn)
        started = time.monotonic()
        output = case.finish(at, 20)
        assert conn.done and 'nc: socket or I/O failure' in output, output
        elapsed = time.monotonic() - started
        assert elapsed < 20
        assert '\nnc-boundary-recovered\n' in case.finish(case.start('echo nc-boundary-recovered'))
    except BaseException:
        case.failure = traceback.format_exc(); case.diagnostics()
    finally:
        # Intentional incomplete stream uses the boundary audit below, not the
        # complete-transfer auditor. Preserve all raw injection/capture files.
        if case.peer: case.peer.connections.clear()
        case.close()
    try:
        captured = list(records(case.pcap))
        injected = [bytes.fromhex(json.loads(line)['hex']) for line in
                    (root / 'injection.jsonl').read_text().splitlines()]
        inbound = [raw for _, raw in captured if raw[6:12] == bytes.fromhex('020304050607')]
        assert sorted(inbound) == sorted(injected), 'capture/injection disagreement'
        packets = [p for _, raw in captured if (p := parse(raw)) is not None]
        assert sum(len(p['data']) for p in packets if p['source'] == 40001) >= 8192
        assert any(p['source'] == 40001 and p['flags'] & 4 for p in packets)
        assert not any(p['source'] == 9000 and p['flags'] & 1 for p in packets)
    except BaseException:
        result = json.loads((root / 'result.json').read_text())
        result.update(status='FAIL', error=traceback.format_exc())
        save(root / 'result.json', result)
        save(root / 'nc-boundary.json', result)
        raise
    evidence = dict(status='PASS', guest_elapsed=elapsed, peer_deadline_seconds=3,
                    acknowledged_response_bytes=conn.una - 1, guest_receive_window=conn.window,
                    request_complete=False, peer_consumed_request_bytes=0)
    save(root / 'nc-boundary.json', evidence)
    print('[PASS]', root, evidence, flush=True)


def run(root, early):
    if early: return early_run(root)
    case = Case(root, 'bios', 'e1000', 'user')
    case.config = dict(profiles=['nc-large-request'])
    request = (REPO / 'build/nc.elf').read_bytes()
    assert len(request) > 8192, 'fixture must exceed a full TCP buffer/window'
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0)); listener.listen(); listener.settimeout(180)
    port = listener.getsockname()[1]
    evidence = dict(request_bytes=len(request), early=False, peer_reads_request=True)

    def peer():
        with listener.accept()[0] as sock:
            sock.settimeout(30); received = bytearray()
            while True:
                part = sock.recv(4096)
                if not part: break
                received.extend(part)
                assert len(received) <= len(request)
            assert bytes(received) == request, 'nc large request byte mismatch'
            evidence['received_bytes'] = len(received)
            sock.sendall(b'nc-large-response\n'); sock.shutdown(socket.SHUT_WR)

    try:
        case.boot()
        # Separate the reboot quiet period from the boundary deadlines.
        quiet_end = time.monotonic() + 125
        case.wait(lambda _: time.monotonic() >= quiet_end, 130)
        with concurrent.futures.ThreadPoolExecutor(1) as pool:
            future = pool.submit(peer)
            started = time.monotonic()
            command = f'nc 10.0.2.2 {port} < /bin/nc'
            at = case.start(command)
            output = case.finish(at, 180)
            future.result(timeout=1)
            evidence['guest_elapsed'] = time.monotonic() - started
            assert '\nnc-large-response\n' in output and 'nc: ' not in output, output
            case.expected.append(dict(port=port, guest_port=None, expected=request.hex(),
                                      response=b'nc-large-response\n'.hex(), active=False))
            # Prompt recovery plus another actual command, not just QEMU exit.
            assert '\nnc-boundary-recovered\n' in case.finish(case.start('echo nc-boundary-recovered'))
    except BaseException:
        case.failure = traceback.format_exc(); case.diagnostics()
    finally:
        listener.close(); case.close()
    try:
        packets = [p for _, raw in records(case.pcap) if (p := parse(raw)) is not None
                   and port in (p['source'], p['dest'])]
        guest_data = [p for p in packets if p['dest'] == port and p['data']]
        assert len(guest_data) > 1, 'request must span multiple wire segments'
        evidence.update(status='PASS', guest_data_segments=len(guest_data),
                        wire_gate='independent exact stream audit')
    except BaseException:
        evidence.update(status='FAIL', error=traceback.format_exc())
        result = json.loads((root / 'result.json').read_text())
        result.update(status='FAIL', error=evidence['error'])
        save(root / 'result.json', result)
        raise
    finally:
        save(root / 'nc-boundary.json', evidence)
    print('[PASS]', root, evidence, flush=True)


if __name__ == '__main__':
    root = REPO / 'build/net2-step5' / uuid.uuid4().hex[:8]
    print('Artifacts:', root, flush=True)
    run(root / 'nc-large', False)
    run(root / 'nc-early-reset', True)
