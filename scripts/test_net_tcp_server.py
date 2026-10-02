#!/usr/bin/env python3
"""Step 4 Ring 3 listener via SLIRP host forwarding; disposable ISO, no data disks."""
import argparse
import re
import shutil
import socket
import struct
import subprocess
import tempfile
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
        assert checksum(ip[12:20] + struct.pack('!BBH', 0, 6, len(tcp)) + tcp) == 0
        header = (tcp[12] >> 4) * 4
        assert 20 <= header <= len(tcp) and not any(frame[14 + length:])
        source, dest = struct.unpack_from('!HH', tcp)
        seq = struct.unpack_from('!I', tcp, 4)[0]
        if tcp[13] & 2:
            streams[(source, dest)] = {'isn': seq, 'data': {}, 'fin': False}
        if (source, dest) not in streams:
            continue
        stream = streams[(source, dest)]
        offset = (seq - stream['isn'] - 1) & 0xffffffff
        for i, value in enumerate(tcp[header:]):
            assert stream['data'].setdefault(offset + i, value) == value
        if tcp[13] & 1:
            stream['fin'] = True
    expected = bytes((i * 31) & 255 for i in range(65536))
    assert any(s['fin'] and len(s['data']) == 65536 and
               bytes(s['data'][i] for i in range(65536)) == expected for s in streams.values())


def run(mode, model, cpus, iso, tmp):
    label = f'{mode}-{model}-user-smp{cpus}'
    root = tmp / label; root.mkdir()
    log = root / 'serial.log'; log.write_text('')
    uart_path = root / 'uart'; pcap = root / 'wire.pcap'
    with socket.socket() as port_probe:
        port_probe.bind(('127.0.0.1', 0)); port = port_probe.getsockname()[1]
    cmd = ['qemu-system-x86_64', '-M', 'q35', '-m', '2G', '-accel', 'tcg', '-smp', str(cpus),
           '-display', 'none', '-monitor', 'none', '-no-reboot', '-boot', 'd', '-cdrom', str(iso),
           '-chardev', f'socket,id=uart,path={uart_path},server=on,wait=off,logfile={log}', '-serial', 'chardev:uart',
           '-netdev', f'user,id=net0,hostfwd=tcp:127.0.0.1:{port}-10.0.2.15:9000',
           '-device', f'{model},netdev=net0,mac=52:54:00:12:34:56',
           '-object', f'filter-dump,id=dump0,netdev=net0,file={pcap}']
    if mode == 'uefi':
        variables = root / 'vars.fd'; shutil.copyfile(VARS, variables)
        cmd += ['-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
                '-drive', f'if=pflash,format=raw,unit=1,file={variables}']
    expected_cmd = tuple(cmd)
    def preflight(candidate): assert tuple(candidate) == expected_cmd, 'unauthorized QEMU argv/storage'
    preflight(cmd)
    for extra in (['-drive', 'file=unsafe.img'], ['-device', 'nvme'], ['-blockdev', 'driver=file,filename=unsafe.img']):
        try: preflight(cmd + extra)
        except AssertionError: pass
        else: raise AssertionError('unsafe argv accepted')
    proc = uart = None
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
            def wait(predicate, timeout=90):
                until = time.monotonic() + timeout
                while time.monotonic() < until:
                    try: uart.recv(65536)
                    except socket.timeout: pass
                    t = text()
                    if predicate(t): return t
                    assert proc.poll() is None, (root / 'stderr.log').read_text()
                raise AssertionError(text()[-7000:])
            def start(line):
                at = len(text())
                for b in line.encode() + b'\n': uart.sendall(bytes([b])); time.sleep(.025)
                return at
            def execute(line):
                at = start(line)
                if line.endswith('&'): return wait(lambda t: re.search(r'\[\d+\] \d+', t[at:]))[at:]
                return wait(lambda t: t[at:].rstrip().endswith('fortress>') and '\n' in t[at:])[at:]
            def peer(size=65536):
                body = bytes((i * 31) & 255 for i in range(size))
                with socket.create_connection(('127.0.0.1', port), timeout=20) as connection:
                    connection.settimeout(25)
                    # Drain each bounded echo while sending: never impose a
                    # request-then-response deadlock on the streaming fixture.
                    for at in range(0, size, 1024):
                        chunk = body[at:at+1024]; connection.sendall(chunk); answer = b''
                        while len(answer) < len(chunk):
                            part = connection.recv(len(chunk)-len(answer)); assert part
                            answer += part
                        assert answer == chunk
                    connection.shutdown(socket.SHUT_WR); assert connection.recv(1) == b''
            wait(lambda t: 'fortress> ' in t and 'Gateway ARP resolved' in t, 120)
            if cpus == 4: wait(lambda t: 'AP socket dispatch rejection PASS' in t)
            at = start('tcpserve 9000')
            wait(lambda t: 'TCP server listening' in t[at:]); peer()
            wait(lambda t: 'TCP finite server / accept ABI / child independence PASS' in t[at:] and t[at:].rstrip().endswith('fortress>'))
            at = start('tcpserve 9000 --caught')
            wait(lambda t: 'TCP caught accept ready' in t[at:]); uart.sendall(b'\x03')
            wait(lambda t: 'TCP caught accept PASS' in t[at:] and t[at:].rstrip().endswith('fortress>'))
            at = start('tcpserve 9000 --shared')
            wait(lambda t: 'TCP shared listener parent closed' in t[at:] and 'TCP server listening' in t[at:]); peer(4096)
            wait(lambda t: 'TCP shared listener PASS' in t[at:] and t[at:].rstrip().endswith('fortress>'))
            at = start('tcpserve 9000 --compete')
            wait(lambda t: t[at:].count('TCP server listening') >= 2)
            peer(8192); peer(8192)
            wait(lambda t: 'TCP competing acceptors PASS' in t[at:] and t[at:].rstrip().endswith('fortress>'))
            stop_at = len(text())
            launch = execute('tcpserve 9000 &'); job = re.search(r'\[(\d+)\] \d+', launch); assert job
            wait(lambda t: 'TCP server listening' in t[stop_at:])
            execute(f'kill %{job[1]} STOP'); time.sleep(9); execute(f'kill %{job[1]} CONT')
            peer(4096); wait(lambda t: 'child independence PASS' in t[stop_at:])
            wait(lambda t: t[stop_at:].rstrip().endswith('fortress>'))
            kill_at = len(text())
            launch = execute('tcpserve 9000 &'); job = re.search(r'\[(\d+)\] \d+', launch); assert job
            wait(lambda t: 'TCP server listening' in t[kill_at:])
            execute(f'kill %{job[1]} KILL'); time.sleep(.5)
            at = start('tcpserve 9000'); wait(lambda t: 'TCP server listening' in t[at:]); peer(4096)
            wait(lambda t: 'child independence PASS' in t[at:] and t[at:].rstrip().endswith('fortress>'))
            result = execute('ping -c 1 10.0.2.2'); assert '1 probes, 1 replies' in result, result
            assert 'TCP finite server FAIL' not in text() and '[FATAL]' not in text() and 'PANIC' not in text()
    finally:
        if uart is not None: uart.close()
        if proc is not None:
            proc.terminate()
            try: proc.wait(timeout=3)
            except subprocess.TimeoutExpired: proc.kill(); proc.wait()
        for path in (log, pcap, root / 'stderr.log'):
            if path.exists(): shutil.copyfile(path, REPO / 'build' / f'net2-step4-{label}-{path.name}')
    audit(pcap)
    print(f'[PASS] {label}: finite 64KiB server / ACCEPT ABI / caught SIGINT / shared listener / competing acceptors / STOP-CONT / KILL recovery / pcap', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--case', choices=[f'{m}-{n}' for m in ('bios', 'uefi') for n in ('e1000', 'e1000e')])
    parser.add_argument('--cpus', type=int, choices=(1,4), default=1)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='fortress-tcp-server-') as folder:
        tmp = Path(folder); iso = build_iso(tmp, 'net_test=arp net_test=tcp')
        if args.case:
            mode, model = args.case.split('-'); run(mode, model, args.cpus, iso, tmp)
        else:
            for mode in ('bios', 'uefi'):
                for model in ('e1000', 'e1000e'): run(mode, model, 1, iso, tmp)
            run('bios', 'e1000', 4, iso, tmp)
            print('NET-2 Step 4 server 5/5 PASS; physical acceptance remains pending.', flush=True)


if __name__ == '__main__': main()
