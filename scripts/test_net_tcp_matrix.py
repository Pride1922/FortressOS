#!/usr/bin/env python3
"""NET-2 Step 5 persistent, disposable, no-data-disk TCP fixture and matrix."""
import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import time
import threading
import traceback
import uuid
from pathlib import Path
from test_net_eth import build_iso
from test_net_pci import REPO, CODE, VARS
from net_tcp_socket_peer import Peer
from net_tcp_wire_audit import audit, records, parse

BODY = bytes((i * 31) & 255 for i in range(65536))
FIXTURE_SOURCES = {name: (REPO / 'scripts' / name).read_bytes() for name in
                   ('test_net_tcp_matrix.py', 'net_tcp_socket_peer.py', 'net_tcp_wire_audit.py')}


def save(path, value):
    with path.open('w') as stream:
        json.dump(value, stream, indent=2); stream.write('\n'); stream.flush(); os.fsync(stream.fileno())


class Case:
    def __init__(self, root, mode, model, backend, cpus=1):
        self.root, self.mode, self.model, self.backend, self.cpus = root, mode, model, backend, cpus
        root.mkdir(parents=True, exist_ok=False)
        self.proc = self.uart = self.host = self.peer = None
        self.files = []; self.expected = []; self.audit_result = None; self.audit_error = None
        self.config = {}
        self.serial_buffer = bytearray()
        self.serial_lock = threading.Lock()
        self.serial_written = 0
        self.reader_stop = threading.Event()
        self.reader_error = None
        self.reader = None
        self.log = root / 'serial.log'; self.log.write_text('')
        self.serial = self.log.open('ab', buffering=65536); self.files.append(self.serial)
        self.serial_flush = time.monotonic()
        self.pcap = root / 'wire.pcap'
        self.failure = None

    def boot(self):
        iso = build_iso(self.root, 'net_test=arp net_test=tcp')
        # drvfs cannot bind AF_UNIX sockets; evidence files still live directly
        # in the persistent case directory. Only this IPC endpoint is ephemeral.
        path = Path('/tmp') / ('fortress-net5-' + uuid.uuid4().hex[:12])
        self.uart_path = path
        self.qmp_path = Path(str(path) + '-qmp')
        assert len(str(path).encode()) < 108
        self.host = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.host.bind(('127.0.0.1', 0))
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(('127.0.0.1', 0)); guest_port = probe.getsockname()[1]
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0)); self.forward = probe.getsockname()[1]
        if self.backend == 'socket':
            injections = (self.root / 'injection.jsonl').open('w')
            events = (self.root / 'peer-events.jsonl').open('w')
            self.files += [injections, events]
            self.peer = Peer(self.host, ('127.0.0.1', guest_port), injections, events)
            net = f'socket,id=net0,udp=127.0.0.1:{self.host.getsockname()[1]},localaddr=127.0.0.1:{guest_port}'
        else:
            for name in ('injection.jsonl', 'peer-events.jsonl'): (self.root / name).write_text('')
            net = f'user,id=net0,hostfwd=tcp:127.0.0.1:{self.forward}-10.0.2.15:9000'
        cmd = ['qemu-system-x86_64', '-M', 'q35', '-m', '2G', '-accel', 'tcg', '-smp', str(self.cpus),
               '-display', 'none', '-monitor', 'none', '-no-reboot', '-boot', 'd', '-cdrom', str(iso),
               '-chardev', f'socket,id=uart,path={path},server=on,wait=on',
               '-serial', 'chardev:uart', '-qmp', f'unix:{self.qmp_path},server=on,wait=off', '-netdev', net,
               '-device', f'{self.model},netdev=net0,mac=52:54:00:12:34:56',
               '-object', f'filter-dump,id=dump0,netdev=net0,file={self.pcap}']
        if self.mode == 'uefi':
            variables = self.root / 'vars.fd'; shutil.copyfile(VARS, variables)
            cmd += ['-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
                    '-drive', f'if=pflash,format=raw,unit=1,file={variables}']
        frozen = tuple(cmd)
        def preflight(candidate): assert tuple(candidate) == frozen, 'unauthorized QEMU argv/storage'
        preflight(cmd)
        for extra in (['-drive', 'file=unsafe.img'], ['-device', 'nvme'], ['-blockdev', 'driver=file,filename=unsafe.img'], ['-device', 'usb-storage,drive=x']):
            try: preflight(cmd + extra)
            except AssertionError: pass
            else: raise AssertionError('unsafe argv accepted')
        save(self.root / 'argv.json', cmd)
        save(self.root / 'scenario.json', dict(mode=self.mode, model=self.model, backend=self.backend, cpus=self.cpus,
             host_port=self.host.getsockname()[1], guest_port=guest_port, forward_port=self.forward,
             packet_limit=20000, connection_limit=8, traffic_deadline=120, quiet_warmup_limit=180,
             guest_ip='10.0.2.15', peer_ip='10.0.2.2', guest_mac='52:54:00:12:34:56',
             peer_mac='02:03:04:05:06:07', seed=None, **self.config))
        versions = subprocess.check_output(['qemu-system-x86_64', '--version'], text=True)
        versions += subprocess.check_output(['python3', '--version'], text=True)
        versions += 'golden sha256 ' + hashlib.sha256((REPO / 'tests/fixtures/net_tcp_wire_vectors.json').read_bytes()).hexdigest() + '\n'
        versions += 'git ' + subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=REPO, text=True)
        sources = self.root / 'fixture-sources'; sources.mkdir()
        for name, raw in FIXTURE_SOURCES.items():
            (sources / name).write_bytes(raw)
            versions += name + ' sha256 ' + hashlib.sha256(raw).hexdigest() + '\n'
        (self.root / 'versions.txt').write_text(versions)
        stderr = (self.root / 'qemu-stderr.log').open('wb'); self.files.append(stderr)
        self.proc = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=stderr)
        end = time.monotonic() + 20
        while time.monotonic() < end:
            try:
                self.uart = socket.socket(socket.AF_UNIX); self.uart.connect(str(path)); break
            except OSError:
                self.uart.close(); self.uart = None; time.sleep(.03)
        assert self.uart is not None, 'UART creation timeout'
        self.uart.settimeout(.05)
        self.start_reader()
        self.wait(lambda t: re.search(r'(?:fortress> |fortress:[^\r\n]* \$ )', t) and 'Gateway ARP resolved' in t, 120)
        if self.cpus == 4: self.wait(lambda t: 'AP socket dispatch rejection PASS' in t)

    def start_reader(self):
        # Drain independently of drvfs writes and packet/audit processing. A
        # delayed host consumer can otherwise trip the guest's deliberately
        # bounded UART transmit wait and latch serial output unavailable.
        def drain():
            try:
                while not self.reader_stop.is_set():
                    try: data = self.uart.recv(65536)
                    except socket.timeout: continue
                    if not data: break
                    with self.serial_lock:
                        assert len(self.serial_buffer) + len(data) <= 1048576, 'serial evidence budget exceeded'
                        self.serial_buffer.extend(data)
            except Exception:
                if not self.reader_stop.is_set(): self.reader_error = traceback.format_exc()
        self.reader = threading.Thread(target=drain, name='bounded-uart-drain', daemon=True)
        self.reader.start()

    def text(self):
        with self.serial_lock: raw = bytes(self.serial_buffer)
        return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]', '', raw.decode(errors='replace')).replace('\r', '')

    def flush_serial(self):
        with self.serial_lock:
            data = bytes(self.serial_buffer[self.serial_written:]); self.serial_written = len(self.serial_buffer)
        if data: self.serial.write(data)
        self.serial.flush(); self.serial_flush = time.monotonic()

    def pump(self):
        if self.peer: self.peer.poll()
        assert self.reader_error is None, self.reader_error
        if time.monotonic() - self.serial_flush >= .25:
            self.flush_serial()
        assert self.proc.poll() is None, 'QEMU exited: ' + (self.root / 'qemu-stderr.log').read_text(errors='replace')
        time.sleep(.005)

    def wait(self, predicate, timeout=120):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.pump()
            text = self.text()
            if predicate(text): return text
        raise TimeoutError('case deadline: ' + self.text()[-4000:])

    def start(self, command):
        at = len(self.text())
        for byte in command.encode() + b'\n':
            self.uart.sendall(bytes([byte])); time.sleep(.025); self.pump()
        return at

    def finish(self, at, timeout=120):
        return self.wait(lambda t: '\n' in t[at:] and re.search(r'(?:fortress>|(?:\[-?\d+\] )?fortress:[^\r\n]* \$)\s*$', t[at:]), timeout)[at:]

    def client(self, port, profile='clean', mss=536):
        if self.peer: self.peer.listen(port, BODY, BODY, profile, mss)
        else:
            self.app_listener = socket.socket(); self.app_listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.app_listener.bind(('127.0.0.1', 0)); port = self.app_listener.getsockname()[1]
            self.app_listener.listen(); self.app_listener.settimeout(180)
            self.pool = concurrent.futures.ThreadPoolExecutor(max_workers=1)
            def responder():
                with self.app_listener.accept()[0] as sock:
                    sock.settimeout(120); received = bytearray()
                    while True:
                        part = sock.recv(4096)
                        if not part: break
                        received.extend(part); assert len(received) <= 65536
                    assert bytes(received) == BODY
                    sock.sendall(BODY); sock.shutdown(socket.SHUT_WR)
            self.future = self.pool.submit(responder)
        warmup = time.monotonic() + 180
        while True:
            at = self.start(f'tcptest 10.0.2.2 {port}')
            result = self.finish(at)
            if 'tcptest: error 21' not in result: break
            assert time.monotonic() < warmup, 'real guest reboot quiet period did not end'
            until = time.monotonic() + 10
            while time.monotonic() < until: self.pump()
        assert 'TCP 65536 bytes each direction / ABI / dup / read-write / half-close PASS' in result, result
        if self.peer:
            self.wait(lambda t: any(c.port == port and c.done for c in self.peer.connections))
        else:
            self.future.result(timeout=1); self.app_listener.close(); self.pool.shutdown()
            self.expected.append(dict(port=port, guest_port=None, expected=BODY.hex(), response=BODY.hex(), active=False))

    def server(self, profile='clean', mss=536):
        at = self.start('tcpserve 9000')
        self.wait(lambda t: 'TCP server listening' in t[at:])
        if self.peer:
            conn = self.peer.active(40000, BODY, BODY, profile=profile, mss=mss)
            self.wait(lambda t: conn.done)
        else:
            with socket.create_connection(('127.0.0.1', self.forward), timeout=20) as sock:
                sock.settimeout(60)
                for index in range(0, len(BODY), 1024):
                    chunk = BODY[index:index + 1024]; sock.sendall(chunk); answer = bytearray()
                    while len(answer) < len(chunk):
                        part = sock.recv(len(chunk) - len(answer)); assert part; answer.extend(part)
                    assert bytes(answer) == chunk
                sock.shutdown(socket.SHUT_WR); assert sock.recv(1) == b''
            self.expected.append(dict(port=None, guest_port=9000, expected=BODY.hex(), response=BODY.hex(), active=True))
        result = self.finish(at)
        assert 'TCP finite server / accept ABI / child independence PASS' in result, result

    def backlog(self):
        at = self.start('tcpserve 9000 --compete')
        self.wait(lambda t: t[at:].count('TCP server listening') >= 2)
        pending = [self.peer.active(40000 + i, b'', b'', hold=True) for i in range(4)]
        self.wait(lambda t: all(c.irs is not None for c in pending))
        self.peer.inject(pending[0].wire(2, at=0))
        from net_tcp_socket_peer import frame
        self.peer.inject(frame(40004, 9000, 123, 0, 2, mss=536), 'overflow')
        until = time.monotonic() + 2
        while time.monotonic() < until: self.pump()
        assert 40004 not in self.peer.syn_seen, 'backlog overflow answered SYN'
        first = pending[1]  # Complete a non-head queue member.
        first.expected = first.response = BODY; first.hold = False
        first.send(16); first.pump()
        self.wait(lambda t: first.done)
        self.wait(lambda t: 'child independence PASS' in t[at:])
        # Guest absolute half-open deadline is 30s; host delay is only a lower
        # bound. A changed guest ISN on the repeated tuple is the expiry proof.
        until = time.monotonic() + 35
        while time.monotonic() < until: self.pump()
        recovered = pending[2]; old_isn = recovered.irs
        assert recovered.state == 'EXPIRED', 'half-open deadline reset not observed'
        recovered.irs = None; recovered.state = 'SYN'; recovered.faults.add('restart')
        recovered.started = time.monotonic()
        self.peer.inject(recovered.wire(2, at=0))
        self.wait(lambda t: recovered.irs is not None)
        assert recovered.irs != old_isn, 'half-open did not expire and recreate'
        recovered.expected = recovered.response = BODY; recovered.hold = False
        recovered.send(16); recovered.pump()
        self.wait(lambda t: recovered.done)
        assert 'TCP competing acceptors PASS' in self.finish(at)
        self.peer.event('backlog-gate', cap=4, overflow=40004, nonhead=40001,
                        recovered=40002, old_isn=old_isn, new_isn=recovered.irs)

    def nc(self):
        def linux_reply(listener, expected, response):
            with listener.accept()[0] as sock:
                sock.settimeout(30); received = bytearray()
                while True:
                    part = sock.recv(4096)
                    if not part: break
                    received.extend(part); assert len(received) <= len(expected)
                assert bytes(received) == expected
                sock.sendall(response); sock.shutdown(socket.SHUT_WR)
        for port, expected, response in ((7784, b'hello\n', b'client-response\n'), (7785, b'', b'empty-response\n')):
            listener = pool = future = None
            try:
                if self.peer: self.peer.listen(port, expected, response)
                else:
                    listener = socket.socket(); listener.bind(('127.0.0.1', 0)); port = listener.getsockname()[1]
                    listener.listen(); listener.settimeout(30)
                    pool = concurrent.futures.ThreadPoolExecutor(1)
                    future = pool.submit(linux_reply, listener, expected, response)
                command = f'echo hello | nc 10.0.2.2 {port}' if expected else f'nc 10.0.2.2 {port} < /dev/null'
                at = self.start(command); result = self.finish(at)
                assert '\n' + response.decode() in result and 'nc: ' not in result, result
                if self.peer: self.wait(lambda t: any(c.port == port and c.done for c in self.peer.connections))
                else:
                    future.result(timeout=1)
                    self.expected.append(dict(port=port, guest_port=None, expected=expected.hex(), response=response.hex(), active=False))
            finally:
                if listener: listener.close()
                if pool: pool.shutdown(wait=False, cancel_futures=True)
        at = self.start('echo listener-input | nc -l 9000')
        if self.peer:
            conn = self.peer.active(40001, b'listener-input\n', b'peer-response\n', profile='serial')
            self.wait(lambda t: conn.done)
        else:
            end = time.monotonic() + 20
            while True:
                try: sock = socket.create_connection(('127.0.0.1', self.forward), timeout=2); break
                except OSError:
                    assert time.monotonic() < end; self.pump()
            with sock:
                sock.settimeout(30); received = bytearray()
                while True:
                    part = sock.recv(4096)
                    if not part: break
                    received.extend(part); assert len(received) <= 15
                assert bytes(received) == b'listener-input\n'
                sock.sendall(b'peer-response\n'); sock.shutdown(socket.SHUT_WR)
            self.expected.append(dict(port=None, guest_port=9000, expected=b'listener-input\n'.hex(), response=b'peer-response\n'.hex(), active=True))
        result = self.finish(at)
        assert '\npeer-response\n' in result and 'nc: ' not in result, result

    def close(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try: self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired: self.proc.kill(); self.proc.wait(timeout=3)
        self.reader_stop.set()
        if self.uart:
            try: self.uart.shutdown(socket.SHUT_RDWR)
            except OSError: pass
            self.uart.close()
        if self.reader:
            self.reader.join(timeout=1)
            if self.reader.is_alive(): self.failure = (self.failure or '') + '\nUART drain did not stop'
        if self.reader_error: self.failure = (self.failure or '') + '\nUART drain: ' + self.reader_error
        self.flush_serial()
        if self.host: self.host.close()
        if hasattr(self, 'uart_path'): self.uart_path.unlink(missing_ok=True)
        if hasattr(self, 'qmp_path'): self.qmp_path.unlink(missing_ok=True)
        for stream in self.files:
            stream.flush(); os.fsync(stream.fileno()); stream.close()
        if hasattr(self, 'app_listener'): self.app_listener.close()
        if hasattr(self, 'pool'): self.pool.shutdown(wait=False, cancel_futures=True)
        scenarios = self.peer.summaries() if self.peer else self.expected
        if not self.peer and scenarios:
            for _, raw in records(self.pcap):
                if raw[6:12] != bytes.fromhex('525400123456'): continue
                packet = parse(raw)
                if packet is None or not packet['flags'] & 2: continue
                for spec in scenarios:
                    if spec['active'] and packet['source'] == 9000 and spec['port'] is None:
                        # Distinct passive connections share guest listener port;
                        # consume their first SYN/ACK tuple in scenario order.
                        claimed = {s['port'] for s in scenarios if s['active'] and s['port'] is not None}
                        if packet['dest'] not in claimed:
                            spec['port'] = packet['dest']; break
                    elif not spec['active'] and packet['dest'] == spec['port'] and spec['guest_port'] is None:
                        claimed = {(s['guest_port'],s['port']) for s in scenarios if not s['active'] and s['guest_port'] is not None}
                        if (packet['source'],packet['dest']) not in claimed:
                            spec['guest_port'] = packet['source']; break
        save(self.root / 'streams.json', scenarios)
        try:
            if scenarios: self.audit_result = audit(self.pcap, self.root / 'injection.jsonl' if self.peer else None, scenarios)
        except Exception:
            self.audit_error = traceback.format_exc()
            self.failure = (self.failure or '') + '\nIndependent audit: ' + self.audit_error
        manifest = dict(status='FAIL' if self.failure else 'PASS', error=self.failure, audit=self.audit_result,
                        audit_error=self.audit_error,
                        pid=None if self.proc is None else self.proc.pid,
                        exit_status=None if self.proc is None else self.proc.returncode,
                        reaped=self.proc is None or self.proc.poll() is not None,
                        streams=scenarios, artifacts={name: (self.root / name).exists() for name in
                        ['serial.log', 'qemu-stderr.log', 'wire.pcap', 'injection.jsonl', 'peer-events.jsonl',
                         'argv.json', 'scenario.json', 'versions.txt', 'arp.iso', 'vars.fd']})
        save(self.root / 'result.json', manifest)
        if self.failure: raise AssertionError(self.failure)

    def diagnostics(self):
        """Read-only VM state on failure; never alters guest execution/locks."""
        if self.proc is None or self.proc.poll() is not None: return
        data = {}
        try:
            with socket.socket(socket.AF_UNIX) as monitor:
                monitor.settimeout(2); monitor.connect(str(self.qmp_path))
                reader = monitor.makefile('rb')
                json.loads(reader.readline())
                def query(command):
                    monitor.sendall(json.dumps(command).encode() + b'\n')
                    while True:
                        reply = json.loads(reader.readline())
                        if 'return' in reply or 'error' in reply: return reply
                query({'execute': 'qmp_capabilities'})
                for name in ('info registers', 'info cpus', 'info lapic'):
                    data[name] = query({'execute': 'human-monitor-command', 'arguments': {'command-line': name}})
                reader.close()
        except Exception: data['diagnostic_error'] = traceback.format_exc()
        save(self.root / 'vm-state.json', data)


def run(root, mode, model, backend, cpus, core=False, failure=None, backlog=False):
    case = Case(root, mode, model, backend, cpus)
    save(root / 'runner.json', dict(mode=mode, model=model, backend=backend, cpus=cpus,
                                   core=core, failure=failure, backlog=backlog, seed=None))
    case.config = dict(core=core, backlog=backlog, forced_failure=failure,
                       profiles=['backlog-expiry'] if backlog else
                       ['clean', 'mss1460', 'handshake-loss', 'loss-reorder', 'zero-window', 'fin-loss', 'malformed', 'ecn-final-ack-loss'] if core else
                       ['clean-client', 'clean-server', 'nc-client', 'nc-empty', 'nc-listener'])
    try:
        case.boot()
        if failure == 'failure': raise AssertionError('intentional failure retention gate')
        if failure == 'timeout': case.wait(lambda t: False, .2)
        if failure == 'crash': case.proc.kill(); case.wait(lambda t: False, 1)
        if backlog: case.backlog()
        else:
            case.server('ecn' if core else 'clean', None if core else 536)
            case.client(7777)
        if core and not backlog:
            case.client(7778, mss=1460)
            for port, profile in enumerate(('handshake-loss', 'loss-reorder', 'zero-window', 'fin-loss', 'malformed'), 7779):
                case.client(port, profile)
                connection = case.peer.connections[-1]
                assert connection.faults, 'profile not exercised'
        if not core and not backlog and not failure: case.nc()
        assert 'PANIC' not in case.text() and '[FATAL]' not in case.text()
    except BaseException:
        case.failure = traceback.format_exc()
        case.diagnostics()
    finally:
        case.close()
    print(f'[PASS] {root.name}: client/server independent wire gate', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--core', action='store_true')
    parser.add_argument('--backlog', action='store_true')
    parser.add_argument('--all', action='store_true')
    parser.add_argument('--retention', action='store_true')
    parser.add_argument('--jobs', type=int, choices=(1, 2, 3, 4), default=2)
    parser.add_argument('--case', default='bios-e1000-socket', choices=[f'{m}-{n}-{b}' for m in ('bios', 'uefi') for n in ('e1000', 'e1000e') for b in ('user', 'socket')])
    parser.add_argument('--cpus', type=int, choices=(1, 4), default=1)
    parser.add_argument('--failure', choices=('failure', 'timeout', 'crash'))
    args = parser.parse_args()
    if args.retention:
        for failure in ('failure', 'timeout', 'crash'):
            root = REPO / 'build/net2-step5' / uuid.uuid4().hex[:8] / failure
            try: run(root, 'bios', 'e1000', 'socket', 1, failure=failure)
            except AssertionError: pass
            else: raise AssertionError('forced failure unexpectedly passed')
            result = json.loads((root / 'result.json').read_text())
            assert result['status'] == 'FAIL' and result['reaped']
            assert all(result['artifacts'][n] for n in ('serial.log', 'qemu-stderr.log', 'wire.pcap', 'injection.jsonl', 'peer-events.jsonl', 'argv.json', 'scenario.json', 'versions.txt', 'arp.iso'))
            try: os.kill(result['pid'], 0)
            except ProcessLookupError: pass
            else: raise AssertionError('terminated QEMU remains alive')
            print(f'[PASS] retained {failure}: {root}', flush=True)
        return
    if args.all:
        root = REPO / 'build/net2-step5' / uuid.uuid4().hex[:8]
        cases_s1 = [(m, n, b, 1) for m in ('bios', 'uefi') for n in ('e1000', 'e1000e') for b in ('user', 'socket')]
        cases_s4 = [(m, 'e1000', 'user', 4) for m in ('bios', 'uefi')]
        print('Matrix artifacts:', root, flush=True)
        failures = []
        with concurrent.futures.ProcessPoolExecutor(max_workers=args.jobs) as pool:
            futures = [pool.submit(run, root / f'{m}-{n}-{b}-s{c}', m, n, b, c) for m, n, b, c in cases_s1]
            for future in concurrent.futures.as_completed(futures):
                try: future.result()
                except Exception as error:
                    failures.append(str(error)); print('[FAIL]', str(error), flush=True)
        with concurrent.futures.ProcessPoolExecutor(max_workers=1) as pool:
            futures = [pool.submit(run, root / f'{m}-{n}-{b}-s{c}', m, n, b, c) for m, n, b, c in cases_s4]
            for future in concurrent.futures.as_completed(futures):
                try: future.result()
                except Exception as error:
                    failures.append(str(error)); print('[FAIL]', str(error), flush=True)
        save(root / 'matrix.json', dict(total=10, passed=10-len(failures), failures=failures))
        assert not failures, f'{len(failures)} matrix cases failed; artifacts: {root}'
        print('NET-2 Step 5 TCP matrix 10/10 PASS; no physical acceptance claim.', flush=True)
        return
    root = REPO / 'build/net2-step5' / uuid.uuid4().hex[:8] / args.case
    print('Artifacts:', root, flush=True)
    mode, model, backend = args.case.split('-')
    run(root, mode, model, backend, args.cpus, args.core, args.failure, args.backlog)


if __name__ == '__main__': main()
