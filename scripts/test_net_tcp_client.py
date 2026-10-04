#!/usr/bin/env python3
"""NET-2 step 3: numeric client via independent SLIRP / Linux app, no data disks."""
import argparse
import re
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time
from pathlib import Path
from test_net_eth import build_iso, MAC
from test_net_icmp import checksum
from test_net_rings import packets
from test_net_pci import REPO, CODE, VARS


def audit(path):
    streams = {}
    for frame in packets(path):
        if frame[6:12] != MAC or frame[12:14] != b'\x08\x00' or frame[23] != 6:
            continue
        ip = frame[14:34]
        assert ip[0] == 0x45 and checksum(ip) == 0
        length = struct.unpack_from('!H', ip, 2)[0]
        tcp = frame[34:14 + length]
        assert len(tcp) >= 20 and checksum(ip[12:20] + struct.pack('!BBH', 0, 6, len(tcp)) + tcp) == 0
        header = (tcp[12] >> 4) * 4
        assert 20 <= header <= len(tcp) and not any(frame[14 + length:])
        port = struct.unpack_from('!H', tcp)[0]
        seq = struct.unpack_from('!I', tcp, 4)[0]
        if tcp[13] & 2:
            streams.setdefault(port, {'isn': seq, 'bytes': {}, 'fin': False})
        if port not in streams:
            continue
        stream = streams[port]
        offset = (seq - stream['isn'] - 1) & 0xffffffff
        for i, value in enumerate(tcp[header:]):
            old = stream['bytes'].setdefault(offset + i, value)
            assert old == value, 'conflicting retransmitted payload'
        if tcp[13] & 1:
            stream['fin'] = True
    expected = bytes((i * 31) & 255 for i in range(65536))
    assert any(len(s['bytes']) == 65536 and s['fin'] and
               bytes(s['bytes'][i] for i in range(65536)) == expected for s in streams.values())


def run(mode, model, cpus, iso, tmp, extended):
    label = f'{mode}-{model}-user-smp{cpus}'
    root = tmp / label; root.mkdir()
    log = root / 'serial.log'; log.write_text('')
    pcap = root / 'wire.pcap'; uart_path = root / 'uart'
    variables = root / 'vars.fd'
    if mode == 'uefi': shutil.copyfile(VARS, variables)
    listeners = []; threads = []; errors = []; stop = threading.Event()
    release = threading.Event(); unread_complete = threading.Event(); received = []
    bulk_busy = threading.Event()
    udp_peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_peer.bind(('127.0.0.1', 0)); udp_peer.settimeout(.2)
    udp_port=udp_peer.getsockname()[1]
    def udp_echo():
        while not stop.is_set():
            try: payload, source=udp_peer.recvfrom(4096)
            except socket.timeout: continue
            except OSError: return
            udp_peer.sendto(payload,source)
    udp_thread=threading.Thread(target=udp_echo,daemon=True); udp_thread.start(); threads.append(udp_thread)
    def server(kind):
        listen = socket.socket(); listen.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listen.bind(('127.0.0.1', 0)); listen.listen(8); listen.settimeout(.2); listeners.append(listen)
        port = listen.getsockname()[1]
        def serve():
            while not stop.is_set():
                try: connection, _ = listen.accept()
                except socket.timeout: continue
                except OSError: return
                try:
                    with connection:
                        connection.settimeout(45)
                        if kind == 'hold':
                            while not release.wait(.1) and not stop.is_set(): pass
                        elif kind == 'reset':
                            connection.sendall(bytes(range(32))); time.sleep(.1)
                            connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                        elif kind == 'unread':
                            assert connection.recv(1) == b'\x00'
                            connection.sendall(b'X' * 32768)
                            assert connection.recv(1) == b''
                            unread_complete.set()
                        else:
                            bulk_busy.set()
                            body = bytearray()
                            while True:
                                chunk = connection.recv(65536)
                                if not chunk: break
                                body.extend(chunk); assert len(body) <= 65536
                            expected = bytes((i * 31) & 255 for i in range(65536))
                            assert body == expected; received.append(bytes(body))
                            for i in range(0,len(body),1024):
                                connection.sendall(body[i:i+1024]); time.sleep(.125 if extended else .005)
                            connection.shutdown(socket.SHUT_WR); bulk_busy.clear()
                except (ConnectionResetError, BrokenPipeError):
                    if kind != 'hold': errors.append(f'{kind}: unexpected reset')
                except BaseException as exc: errors.append(exc)
        thread = threading.Thread(target=serve, daemon=True); thread.start(); threads.append(thread)
        return port
    bulk_port = server('bulk'); hold_port = server('hold'); reset_port = server('reset'); unread_port = server('unread')
    cmd = ['qemu-system-x86_64', '-M', 'q35', '-m', '2G', '-accel', 'tcg', '-smp', str(cpus),
           '-display', 'none', '-monitor', 'none', '-no-reboot', '-boot', 'd', '-cdrom', str(iso),
           '-chardev', f'socket,id=uart,path={uart_path},server=on,wait=off,logfile={log}', '-serial', 'chardev:uart',
           '-netdev', 'user,id=net0', '-device', f'{model},netdev=net0,mac=52:54:00:12:34:56',
           '-object', f'filter-dump,id=dump0,netdev=net0,file={pcap}']
    if mode == 'uefi':
        cmd += ['-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
                '-drive', f'if=pflash,format=raw,unit=1,file={variables}']
    expected_cmd = tuple(cmd)
    def preflight(candidate): assert tuple(candidate) == expected_cmd, 'unauthorized QEMU argv/storage'
    preflight(cmd)
    for extra in (['-drive', 'file=unsafe.img'], ['-device', 'nvme'], ['-blockdev', 'driver=file,filename=unsafe.img']):
        try: preflight(cmd + extra)
        except AssertionError: pass
        else: raise AssertionError('unsafe argv accepted')
    proc = None; uart = None
    try:
        with (root / 'stderr.log').open('wb') as stderr:
            proc = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=stderr)
            until = time.monotonic() + 20
            while time.monotonic() < until:
                try:
                    uart = socket.socket(socket.AF_UNIX); uart.connect(str(uart_path)); break
                except OSError:
                    uart.close(); uart = None; time.sleep(.05)
            assert uart is not None; uart.settimeout(.1)
            def text(): return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]', '', log.read_text(errors='replace')).replace('\r', '')
            def wait(predicate, timeout=60):
                until = time.monotonic() + timeout
                while time.monotonic() < until:
                    try: uart.recv(65536)
                    except socket.timeout: pass
                    t = text()
                    if predicate(t): return t
                    assert proc.poll() is None, (root / 'stderr.log').read_text()
                raise AssertionError(text()[-7000:])
            wait(lambda t: re.search(r'(?:fortress> |fortress:[^\r\n]* \$ )',t) and 'Gateway ARP resolved' in t, 120)
            if cpus==4: wait(lambda t: '[NET 5] AP socket dispatch rejection PASS' in t)
            def start(line):
                at = len(text())
                for b in line.encode() + b'\n': uart.sendall(bytes([b])); time.sleep(.025)
                return at
            def execute(line, timeout=60):
                at = start(line)
                if line.endswith('&'): return wait(lambda t: re.search(r'\[\d+\] \d+', t[at:]), timeout)[at:]
                return wait(lambda t: re.search(r'(?:fortress>|(?:\[-?\d+\] )?fortress:[^\r\n]* \$)\s*$',t[at:]) and '\n' in t[at:], timeout)[at:]
            # Policy-independent retry permits the explicit reboot quiet period;
            # no test bypass or changed guest timer. Bound overall warm-up.
            until = time.monotonic() + 180
            while True:
                result = execute(f'tcptest 10.0.2.2 {bulk_port}')
                if 'tcptest: error 21' not in result: break
                assert time.monotonic() < until
                time.sleep(10)
            assert 'TCP 65536 bytes each direction / ABI / dup / read-write / half-close PASS' in result, result
            at = start('tcptest 10.0.2.99 7777 --connect-caught')
            wait(lambda t: 'TCP caught connect ready' in t[at:]); uart.sendall(b'\x03')
            wait(lambda t: 'TCP caught connect PASS' in t[at:] and re.search(r'(?:fortress>|(?:\[-?\d+\] )?fortress:[^\r\n]* \$)\s*$',t[at:]))
            result = execute(f'tcptest 10.0.2.2 {reset_port} --reset')
            assert 'TCP buffered reset PASS' in result, result
            result = execute(f'tcptest 10.0.2.2 {unread_port} --unread')
            assert 'TCP unread close submitted' in result, result
            assert unread_complete.wait(15), errors
            at = start(f'tcptest 10.0.2.2 {hold_port} --caught')
            wait(lambda t: 'TCP caught receive ready' in t[at:]); uart.sendall(b'\x03')
            wait(lambda t: 'TCP caught receive PASS' in t[at:] and re.search(r'(?:fortress>|(?:\[-?\d+\] )?fortress:[^\r\n]* \$)\s*$',t[at:]))
            if extended:
                at=start(f'tcptest 10.0.2.2 {bulk_port} &')
                wait(lambda t: 'TCP client connected' in t[at:]); assert bulk_busy.wait(3)
                result=execute('ping -c 1 10.0.2.2'); assert '1 probes, 1 replies' in result, result
                result=execute(f'udptest 10.0.2.2 {udp_port} tcp-concurrent')
                assert 'UDP echo PASS' in result and bulk_busy.is_set(), result
                wait(lambda t: 'half-close PASS' in t[at:])
                launch = execute(f'tcptest 10.0.2.2 {hold_port} --hold &')
                job = re.search(r'\[(\d+)\] \d+', launch); assert job, launch
                wait(lambda t: 'TCP indefinite receive ready' in t[t.rfind(f'tcptest 10.0.2.2 {hold_port} --hold &'):])
                execute(f'kill %{job[1]} STOP'); time.sleep(9)
                at = len(text()); execute(f'kill %{job[1]} CONT')
                assert 'TCP indefinite receive PASS' not in text()[at:]
                time.sleep(7); release.set()
                wait(lambda t: 'TCP indefinite receive PASS' in t[at:]); release.clear()
                launch = execute(f'tcptest 10.0.2.2 {hold_port} --hold &')
                job = re.search(r'\[(\d+)\] \d+', launch); assert job
                execute(f'kill %{job[1]} KILL'); release.set(); time.sleep(.3); release.clear()
                result = execute(f'tcptest 10.0.2.2 {bulk_port}')
                assert 'half-close PASS' in result, result
            # Independent refused port; never infer refusal from tool timeout.
            with socket.socket() as closed:
                closed.bind(('127.0.0.1', 0)); closed_port = closed.getsockname()[1]
            result = execute(f'tcptest 10.0.2.2 {closed_port}')
            assert 'tcptest: error 26' in result, result
            result = execute('ping -c 1 10.0.2.2'); assert '1 probes, 1 replies' in result, result
            idle=wait(lambda t: '[NET 5] Idle worker CPU ticks/elapsed ticks/hz (hex):' in t)
            print(f'[IDLE] {label}: ' + re.search(r'\[NET 5\] Idle worker CPU ticks[^\n]+',idle)[0],flush=True)
            assert not errors, errors
            assert 'PANIC' not in text() and '[FATAL]' not in text()
    finally:
        stop.set(); release.set()
        if uart is not None: uart.close()
        if proc is not None:
            proc.terminate()
            try: proc.wait(timeout=3)
            except subprocess.TimeoutExpired: proc.kill(); proc.wait()
        for listen in listeners: listen.close()
        udp_peer.close()
        for thread in threads: thread.join(timeout=2)
        for path in (log, pcap, root / 'stderr.log'):
            if path.exists(): shutil.copyfile(path, REPO / 'build' / f'net2-step3-{label}-{path.name}')
    audit(pcap); assert received
    print(f'[PASS] {label}: independent SLIRP TCP / Linux host app / real Ring 3 / 64KiB both ways / ABI / reset / caught signal / unread close / pcap', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--case', choices=[f'{m}-{n}' for m in ('bios', 'uefi') for n in ('e1000', 'e1000e')])
    parser.add_argument('--cpus',type=int,choices=(1,4),default=1)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='fortress-tcp-client-') as folder:
        tmp = Path(folder); iso = build_iso(tmp, 'net_test=arp net_test=tcp')
        if args.case:
            mode, model = args.case.split('-'); run(mode, model, args.cpus, iso, tmp, args.cpus==1)
        else:
            for mode in ('bios', 'uefi'):
                for model in ('e1000', 'e1000e'): run(mode, model, 1, iso, tmp, True)
            run('bios', 'e1000', 4, iso, tmp, False)
            print('NET-2 step 3 client 5/5 PASS; socket backend/listener matrix and physical acceptance remain later steps.', flush=True)


if __name__ == '__main__': main()
