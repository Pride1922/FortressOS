#!/usr/bin/env python3
"""Phase 5a real Ring 3 UDP, independent wire checks, disposable/no data disks."""
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
from test_net_eth import build_iso, arp_frame, MAC, PEER, GUEST_IP
from test_net_icmp import checksum, ip_frame, echo, decode as icmp_decode
from test_net_rings import packets
from test_net_pci import REPO, CODE, VARS


def udp_body(src, dst, sport, dport, data):
    body = struct.pack('!HHHH', sport, dport, 8 + len(data), 0) + data
    pseudo = src + dst + struct.pack('!BBH', 0, 17, len(body))
    value = checksum(pseudo + body) or 65535
    return body[:6] + struct.pack('!H', value) + body[8:]


def udp_frame(src, dst, sport, dport, data, bad=False):
    body = udp_body(src, dst, sport, dport, data)
    if bad:
        body = body[:6] + bytes([body[6] ^ 1]) + body[7:]
    ip = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(body), 0, 0x4000, 64, 17, 0, src, dst)
    ip = ip[:10] + struct.pack('!H', checksum(ip)) + ip[12:]
    frame = MAC + PEER + b'\x08\0' + ip + body
    return frame + b'\0' * max(0, 60 - len(frame))


def decode(frame):
    assert frame[12:14] == b'\x08\0' and frame[14] == 0x45
    ip = frame[14:34]
    total = struct.unpack_from('!H', ip, 2)[0]
    assert 28 <= total <= len(frame) - 14 and checksum(ip) == 0 and ip[9] == 17
    src, dst = ip[12:16], ip[16:20]
    body = frame[34:14 + total]
    sport, dport, length, value = struct.unpack_from('!HHHH', body)
    assert length == len(body) and value != 0
    assert checksum(src + dst + struct.pack('!BBH', 0, 17, length) + body) == 0
    assert not any(frame[14 + total:]), 'nonzero Ethernet padding'
    return src, dst, sport, dport, body[8:]


def command(mode, model, backend, cpus, iso, variables, log, pcap, uart, host_port, guest_port, forward):
    assert mode in ('bios', 'uefi') and model in ('e1000', 'e1000e')
    assert backend in ('user', 'socket') and cpus in (1, 4)
    net = (f'user,id=net0,hostfwd=udp:127.0.0.1:{forward}-10.0.2.15:7777' if backend == 'user'
           else f'socket,id=net0,udp=127.0.0.1:{host_port},localaddr=127.0.0.1:{guest_port}')
    cmd = ['qemu-system-x86_64', '-M', 'q35', '-m', '2G', '-accel', 'tcg', '-smp', str(cpus),
           '-display', 'none', '-monitor', 'none', '-no-reboot', '-boot', 'd', '-cdrom', str(iso),
           '-chardev', f'socket,id=uart,path={uart},server=on,wait=off,logfile={log}', '-serial', 'chardev:uart',
           '-netdev', net, '-device', f'{model},netdev=net0,mac=52:54:00:12:34:56',
           '-object', f'filter-dump,id=dump0,netdev=net0,file={pcap}']
    if mode == 'uefi':
        cmd += ['-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
                '-drive', f'if=pflash,format=raw,unit=1,file={variables}']
    return cmd


def port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def run(mode, model, backend, cpus, iso, tmp):
    label = f'{mode}-{model}-{backend}-smp{cpus}'
    saved_log = REPO / 'build' / f'test-net-udp-{label}.log'
    # Keep per-byte UART logfile writes on Linux tmpfs/filesystem rather than
    # the WSL Windows mount; preserve all artifacts after QEMU is stopped.
    log = tmp / f'test-net-udp-{label}.log'
    log.write_text(''); pcap = log.with_suffix('.pcap')
    variables = tmp / f'v-{label}.fd'; uart_path = tmp / f'u-{label}'
    if mode == 'uefi':
        shutil.copyfile(VARS, variables)
    stop = threading.Event(); silent = threading.Event(); errors = []; responses = []; requests = []
    guest_port, forward = port(), port()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as host:
        host.bind(('127.0.0.1', 0)); host.settimeout(.05)
        host_port = host.getsockname()[1]
        args = mode, model, backend, cpus, iso, variables, log, pcap, uart_path, host_port, guest_port, forward
        cmd = command(*args)
        def preflight(candidate):
            assert candidate == command(*args), 'unauthorized QEMU argv/storage'
        preflight(cmd)
        for extra in (['-drive', 'file=unsafe.img'], ['-device', 'nvme'], ['-blockdev', 'driver=file,filename=unsafe.img']):
            try:
                preflight(cmd + extra)
            except AssertionError:
                pass
            else:
                raise AssertionError('unsafe argv accepted')
        def inject(frame):
            host.sendto(frame, ('127.0.0.1', guest_port))
        def peer():
            try:
                while not stop.is_set():
                    try:
                        data, source = host.recvfrom(65536)
                    except socket.timeout:
                        continue
                    if backend == 'user':
                        requests.append(data)
                        if not silent.is_set(): host.sendto(data, source)
                        continue
                    if data[12:14] == b'\x08\x06' and data[20:22] == b'\0\x01':
                        # Deliberately leave .99 silent for ARP exhaustion/recovery.
                        if data[38:42] != bytes([10, 0, 2, 99]):
                            inject(arp_frame(True, PEER, data[38:42], MAC, GUEST_IP))
                    elif data[12:14] == b'\x08\0' and data[23] == 1:
                        src, dst, body = icmp_decode(data)
                        if body[0] == 8:
                            ident, seq = struct.unpack_from('!HH', body, 4)
                            inject(ip_frame(dst, src, PEER, MAC, echo(0, ident, seq, body[8:])))
                    elif data[12:14] == b'\x08\0' and data[23] == 17:
                        src, dst, sport, dport, payload = decode(data)
                        if sport == 7777:
                            responses.append((dport, payload))
                        else:
                            requests.append(payload)
                            if not silent.is_set():
                                # A corrupted UDP checksum must drop before the valid echo.
                                inject(udp_frame(dst, src, dport, sport, payload, True))
                                inject(udp_frame(dst, src, dport, sport, payload))
            except BaseException as exc:
                errors.append(exc)
        thread = threading.Thread(target=peer, daemon=True); thread.start()
        proc = None; uart = None
        with log.with_suffix('.stderr').open('wb') as stderr:
            try:
                proc = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=stderr)
                end = time.monotonic() + 20
                while time.monotonic() < end:
                    try:
                        uart = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); uart.connect(str(uart_path)); break
                    except OSError:
                        uart.close(); uart = None; time.sleep(.05)
                assert uart is not None
                uart.settimeout(.1)
                def text():
                    return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]', '', log.read_text(errors='replace')).replace('\r', '')
                def wait(predicate, timeout=45):
                    end = time.monotonic() + timeout
                    while time.monotonic() < end:
                        try: uart.recv(65536)
                        except socket.timeout: pass
                        value = text()
                        if predicate(value): return value
                        assert proc.poll() is None, log.with_suffix('.stderr').read_text()
                    raise AssertionError(text()[-7000:])
                wait(lambda t: 'fortress> ' in t and 'Gateway ARP resolved' in t, 120)
                if cpus == 4:
                    wait(lambda t: '[NET 5] AP socket dispatch rejection PASS' in t)
                def start(line):
                    at = len(text())
                    for byte in line.encode() + b'\n':
                        uart.sendall(bytes([byte])); time.sleep(.025)
                    return at
                def execute(line, timeout=45):
                    at = start(line)
                    if line.endswith('&'):
                        return wait(lambda t: re.search(r'\[\d+\] \d+', t[at:]), timeout)[at:]
                    return wait(lambda t: t[at:].rstrip().endswith('fortress>') and '\n' in t[at:], timeout)[at:]
                target = '10.0.2.2'; echo_port = host_port if backend == 'user' else 7778
                result = execute(f'udptest {target} {echo_port} fortress-phase5')
                assert 'UDP echo PASS' in result, result
                result = execute(f'/bin/net-udp-probe {target} {echo_port}')
                assert 'UDP ABI/binary/fd probe PASS' in result, result
                assert 'UDP receive timeout BSP ticks:' in result
                idle = wait(lambda t: '[NET 5] Idle worker CPU ticks/elapsed ticks/hz (hex):' in t)
                print(f'[IDLE] {label}: ' + re.search(r'\[NET 5\] Idle worker CPU ticks[^\n]+', idle)[0], flush=True)
                result = execute('udptest --listen 0 1'); assert 'usage:' in result
                at = start('/bin/net-udp-probe --caught')
                wait(lambda t: 'UDP caught-signal receive ready' in t[at:])
                uart.sendall(b'\x03')
                wait(lambda t: 'UDP caught-signal receive PASS' in t[at:] and t[at:].rstrip().endswith('fortress>'))
                # Incoming application traffic after the worker has slept.
                at = start('udptest --listen 7777 4')
                wait(lambda t: 'UDP listener ready' in t[at:])
                tags = [b'', b'odd-data!', bytes(range(256)) * 5 + b'X' * 192, b'fortress-inbound-4']
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
                    client.settimeout(5)
                    for tag in tags:
                        if backend == 'user':
                            client.sendto(tag, ('127.0.0.1', forward))
                            returned, _ = client.recvfrom(65536); assert returned == tag
                        else:
                            previous = len(responses)
                            peer_ip = bytes([10, 0, 2, 3])
                            inject(udp_frame(peer_ip, GUEST_IP, 5555, 7777, tag, True))
                            inject(udp_frame(peer_ip, GUEST_IP, 5555, 7777, tag))
                            until = time.monotonic() + 5
                            while len(responses) == previous and time.monotonic() < until: time.sleep(.02)
                            assert responses[-1] == (5555, tag) and len(responses) == previous + 1
                wait(lambda t: 'UDP listener PASS' in t[at:] and t[at:].rstrip().endswith('fortress>'))
                silent.set()
                result = execute(f'udptest {target} {echo_port} silent')
                assert 'udptest: error 21' in result, result
                silent.clear()
                result = execute(f'udptest {target} {echo_port} recovery'); assert 'UDP echo PASS' in result
                if backend == 'socket':
                    result = execute('udptest 10.0.2.99 7778 silent-arp')
                    assert 'udptest: error 9' in result, result
                launch = execute('udptest --listen 7777 1 &')
                job = re.search(r'\[(\d+)\] \d+', launch); assert job, launch
                wait(lambda t: 'UDP listener ready' in t[t.rfind('udptest --listen 7777 1 &'):])
                result = execute('udptest --listen 7777 1'); assert 'udptest: error 15' in result, result
                execute(f'kill %{job[1]} KILL')
                # Last descriptor is reaped through existing generic fd cleanup.
                at = start('udptest --listen 7777 1')
                wait(lambda t: 'UDP listener ready' in t[at:])
                uart.sendall(b'\x03')
                wait(lambda t: t[at:].rstrip().endswith('fortress>'))
                if mode == 'bios' and model == 'e1000' and cpus == 1:
                    launch = execute('udptest --listen 7777 1 &')
                    job = re.search(r'\[(\d+)\] \d+', launch); assert job
                    execute(f'kill %{job[1]} STOP')
                    time.sleep(8)
                    at = len(text()); execute(f'kill %{job[1]} CONT')
                    wait(lambda t: 'udptest: error 22' in t[at:])
                    result = execute('udptest --listen 7777 1'); assert 'error 21' in result, result
                result = execute('ping -c 1 10.0.2.2'); assert '1 probes, 1 replies' in result, result
                assert not errors, errors
            finally:
                stop.set()
                if uart is not None: uart.close()
                if proc is not None:
                    proc.terminate()
                    try: proc.wait(timeout=3)
                    except subprocess.TimeoutExpired: proc.kill(); proc.wait()
                thread.join(timeout=2)
                for path in (log, pcap, log.with_suffix('.stderr')):
                    if path.exists(): shutil.copyfile(path, saved_log.with_suffix(path.suffix))
        outbound = [f for f in packets(pcap) if f[6:12] == MAC and f[12:14] == b'\x08\0' and f[23] == 17]
        assert outbound
        decoded = [decode(f) for f in outbound]
        assert {len(d[-1]) for d in decoded} >= {0, 1, 9, 1472}
        assert all(d[0] == GUEST_IP for d in decoded)
        assert 'PANIC' not in text() and '[FATAL]' not in text()
        print(f'[PASS] {label}: real Ring 3 client/listener, binary/ABI/fd fixture, pcap audit, timeout/kill/port recovery', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--case', choices=[f'{m}-{d}-{b}' for m in ('bios', 'uefi')
                                         for d in ('e1000', 'e1000e') for b in ('user', 'socket')])
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='fortress-net-udp-') as name:
        tmp = Path(name); iso = build_iso(tmp, 'net_test=arp net_test=udp')
        if args.case:
            mode, model, backend = args.case.split('-'); run(mode, model, backend, 1, iso, tmp)
        else:
            for mode in ('bios', 'uefi'):
                for model in ('e1000', 'e1000e'):
                    for backend in ('user', 'socket'): run(mode, model, backend, 1, iso, tmp)
            for mode in ('bios', 'uefi'): run(mode, 'e1000', 'user', 4, iso, tmp)
            print('Phase 5a: eight SMP=1 cases + two SMP=4 BSP/AP-rejection smoke cases PASS; physical 5b pending.', flush=True)


if __name__ == '__main__':
    main()
