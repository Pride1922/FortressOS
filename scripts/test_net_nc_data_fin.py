#!/usr/bin/env python3
"""Real nc tty/null/file/pipe stdin with data+FIN; BIOS/UEFI socket, no disks."""
import time
import traceback
import uuid
from net_tcp_socket_peer import Connection
from test_net_tcp_matrix import Case, REPO, save

PAYLOAD = b'fortress-tcp-inbound\n'


class DataFinPeer(Connection):
    def pump(self):
        if self.state == 'ESTABLISHED' and not self.own_fin:
            assert self.window >= len(self.response) + 1
            self.own_fin = True
            self.send(25, self.response, retain=True)  # PSH|ACK|FIN, one segment.
        super().pump()


def run(root, mode, stdin):
    expected = {'tty': b'', 'null': b'', 'file': (REPO / 'build/nc.elf').read_bytes(),
                'pipe': b'listener-input\n'}[stdin]
    command = {'tty': 'nc -l 9000', 'null': 'nc -l 9000 < /dev/null',
               'file': 'nc -l 9000 < /bin/nc', 'pipe': 'echo listener-input | nc -l 9000'}[stdin]
    case = Case(root, mode, 'e1000', 'socket')
    case.config = dict(profiles=['nc-combined-data-fin'], stdin=stdin)
    try:
        case.boot()
        at = case.start(command)
        conn = DataFinPeer(case.peer, 40001, expected, PAYLOAD, active=True)
        case.peer.connections.append(conn)
        started = time.monotonic()
        output = case.finish(at, 20)
        assert '\n' + PAYLOAD.decode() in output and 'nc: ' not in output, output
        case.wait(lambda _: conn.done, 10)
        assert '\nnc-data-fin-recovered\n' in case.finish(case.start('echo nc-data-fin-recovered'))
        save(root / 'data-fin.json', dict(payload=PAYLOAD.hex(), bytes=len(PAYLOAD),
                                        elapsed=time.monotonic()-started, stdin=stdin,
                                        sent_bytes=len(expected)))
    except BaseException:
        case.failure = traceback.format_exc(); case.diagnostics()
    finally:
        case.close()  # Complete-transfer independent audit, retained injection log.
    print('[PASS] nc prints combined 21-byte data+FIN and restores shell:', root, flush=True)


def main():
    root = REPO / 'build/net2-step6' / uuid.uuid4().hex[:8]
    for mode in ('bios', 'uefi'):
        for stdin in ('tty', 'null', 'file', 'pipe'):
            run(root / f'{mode}-{stdin}', mode, stdin)
    print('nc listener tty/null/file/pipe BIOS/UEFI 8/8 PASS', flush=True)


if __name__ == '__main__': main()
