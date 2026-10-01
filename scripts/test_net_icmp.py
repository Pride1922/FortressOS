#!/usr/bin/env python3
"""Phase 4a real Ring 3 ping + independent socket-peer echo; no data disks."""
import re
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time
from pathlib import Path
from test_net_eth import build_iso, arp_frame, MAC, PEER, GUEST_IP, GATEWAY
from test_net_rings import packets
from test_net_pci import REPO, CODE, VARS


def checksum(data):
    data += b'\0' if len(data) % 2 else b''
    total = sum(struct.unpack('!' + 'H' * (len(data) // 2), data))
    while total >> 16:
        total = (total & 65535) + (total >> 16)
    return (~total) & 65535


def ip_frame(src, dst, smac, dmac, body):
    ip = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(body), 0, 0x4000, 64, 1, 0, src, dst)
    ip = ip[:10] + struct.pack('!H', checksum(ip)) + ip[12:]
    frame = dmac + smac + b'\x08\0' + ip + body
    return frame + b'\0' * max(0, 60 - len(frame))


def echo(kind, identifier, seq, data):
    body = struct.pack('!BBHHH', kind, 0, 0, identifier, seq) + data
    return body[:2] + struct.pack('!H', checksum(body)) + body[4:]


def decode(frame):
    assert len(frame) >= 42 and frame[12:14] == b'\x08\0'
    ihl = (frame[14] & 15) * 4
    total = struct.unpack_from('!H', frame, 16)[0]
    assert 20 <= ihl <= total <= len(frame) - 14
    ip = frame[14:14+ihl]
    body = frame[14+ihl:14+total]
    assert checksum(ip) == checksum(body) == 0
    assert ip[9] == 1
    return ip[12:16], ip[16:20], body


def command(mode, model, backend, iso, variables, log, pcap, uart, host_port, guest_port):
    assert mode in ('bios', 'uefi') and model in ('e1000', 'e1000e') and backend in ('user', 'socket')
    net = 'user,id=net0' if backend == 'user' else f'socket,id=net0,udp=127.0.0.1:{host_port},localaddr=127.0.0.1:{guest_port}'
    cmd = ['qemu-system-x86_64', '-M', 'q35', '-m', '2G', '-accel', 'tcg', '-smp', '1',
           '-display', 'none', '-monitor', 'none', '-no-reboot', '-boot', 'd', '-cdrom', str(iso),
           '-chardev', f'socket,id=uart,path={uart},server=on,wait=off,logfile={log}', '-serial', 'chardev:uart',
           '-netdev', net, '-device', f'{model},netdev=net0,mac=52:54:00:12:34:56',
           '-object', f'filter-dump,id=dump0,netdev=net0,file={pcap}']
    if mode == 'uefi':
        cmd += ['-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
                '-drive', f'if=pflash,format=raw,unit=1,file={variables}']
    return cmd


def preflight(cmd, args):
    assert cmd == command(*args), f'unauthorized QEMU argv: {cmd}'


def run(mode, model, backend, iso, tmp):
    label = f'{mode}-{model}-{backend}'
    saved_log = REPO / 'build' / f'test-net-icmp-{label}.log'
    log = tmp / f'test-net-icmp-{label}.log'
    log.write_text('')
    pcap = log.with_suffix('.pcap')
    uart_path = tmp / f'u-{label}'
    variables = tmp / f'v-{label}.fd'
    if mode == 'uefi':
        shutil.copyfile(VARS, variables)
    stop = threading.Event()
    peer_errors = []
    received_echo = []
    silent = threading.Event()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as host:
        host.bind(('127.0.0.1', 0)); host.settimeout(.05)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
            reservation.bind(('127.0.0.1', 0)); guest_port = reservation.getsockname()[1]
        args = (mode, model, backend, iso, variables, log, pcap, uart_path, host.getsockname()[1], guest_port)
        cmd = command(*args)
        preflight(cmd, args)
        for extra in (['-drive', 'file=unsafe.img'], ['-device', 'nvme'], ['-blockdev', 'driver=file,filename=unsafe.img']):
            try:
                preflight(cmd + extra, args)
            except AssertionError:
                pass
            else:
                raise AssertionError('preflight accepted unauthorized storage')
        def send(frame):
            host.sendto(frame, ('127.0.0.1', guest_port))
        def peer():
            try:
                while not stop.is_set():
                    try:
                        frame, _ = host.recvfrom(65536)
                    except socket.timeout:
                        continue
                    if frame[12:14] == b'\x08\x06' and frame[20:22] == b'\0\x01':
                        send(arp_frame(True, PEER, frame[38:42], MAC, GUEST_IP))
                    elif frame[12:14] == b'\x08\0':
                        src, dst, body = decode(frame)
                        if body[0] == 8 and not silent.is_set():
                            # Wrong sequence and damaged checksum must not complete the probe.
                            identifier, seq = struct.unpack_from('!HH', body, 4)
                            send(ip_frame(dst, src, PEER, MAC, echo(0, identifier, seq ^ 1, body[8:])))
                            bad = bytearray(echo(0, identifier, seq, body[8:])); bad[2] ^= 1
                            send(ip_frame(dst, src, PEER, MAC, bytes(bad)))
                            send(ip_frame(dst, src, PEER, MAC, echo(0, identifier, seq, body[8:])))
                        elif body[0] == 0:
                            received_echo.append(frame)
            except BaseException as exc:
                peer_errors.append(exc)
        thread = threading.Thread(target=peer, daemon=True)
        if backend == 'socket':
            thread.start()
        proc = None; uart = None
        with log.with_suffix('.stderr').open('wb') as stderr:
            try:
                proc = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=stderr)
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline:
                    try:
                        uart = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); uart.connect(str(uart_path)); break
                    except OSError:
                        uart.close(); uart = None; time.sleep(.05)
                assert uart is not None, 'UART connection failed'
                uart.settimeout(.1)
                def text():
                    return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]', '', log.read_text(errors='replace')).replace('\r', '')
                def wait(predicate, timeout=45):
                    end = time.monotonic() + timeout
                    while time.monotonic() < end:
                        try:
                            uart.recv(65536)
                        except socket.timeout:
                            pass
                        t = text()
                        if predicate(t): return t
                        assert proc.poll() is None, log.with_suffix('.stderr').read_text()
                    raise AssertionError(text()[-5000:])
                wait(lambda t: 'fortress> ' in t and 'Gateway ARP resolved' in t and
                     'Echo probe reply matched' in t, 120)
                def execute(line, timeout=45):
                    start = len(text())
                    for byte in line.encode() + b'\n':
                        uart.sendall(bytes([byte])); time.sleep(.025)
                    if line.endswith('&'):
                        return wait(lambda t: re.search(r'\[\d+\] \d+', t[start:]) is not None, timeout)[start:]
                    return wait(lambda t: t[start:].rstrip().endswith('fortress>') and '\n' in t[start:], timeout)[start:]
                result = execute('ping -c 4 10.0.2.2')
                assert '4 probes, 4 replies, 0% loss' in result, result
                assert result.count('32 bytes from') == 4, result
                result = execute('ping -c 0 10.0.2.2'); assert 'usage: ping' in result
                result = execute('/bin/net-ping-probe'); assert 'NETCTL ABI probe PASS' in result, result
                if backend == 'socket':
                    # The cold peer 10.0.2.3 is independently ARP-resolved for a 9-byte echo request.
                    peer_ip = bytes([10, 0, 2, 3]); body = echo(8, 0x1234, 7, b'odd-data!')
                    send(ip_frame(peer_ip, GUEST_IP, PEER, MAC, body))
                    end = time.monotonic() + 5
                    while not received_echo and time.monotonic() < end: time.sleep(.05)
                    assert received_echo, 'no inbound-request echo reply after idle ticks'
                    src, dst, returned = decode(received_echo[-1])
                    assert src == GUEST_IP and dst == peer_ip
                    assert returned == echo(0, 0x1234, 7, b'odd-data!'), 'echo reply bytes differ'
                    silent.set()
                    result = execute('ping -c 1 -W 1 10.0.2.2')
                    assert 'echo timeout' in result and '1 probes, 0 replies, 100% loss' in result, result
                    silent.clear()
                    result = execute('ping -c 1 10.0.2.2'); assert '1 probes, 1 replies' in result
                    silent.set()
                    launch = execute('ping -c 1 -W 5 10.0.2.2 &')
                    job = re.search(r'\[(\d+)\] \d+', launch)
                    assert job, launch
                    result = execute('ping -c 1 10.0.2.2')
                    assert 'control error 21' in result, 'competing caller did not report busy: ' + result
                    execute(f'kill %{job[1]} KILL')
                    # No owner-exit hook: expired lease must free the mailbox even after KILL.
                    time.sleep(12)
                    silent.clear()
                    result = execute('ping -c 1 10.0.2.2')
                    assert '1 probes, 1 replies' in result, 'abandoned owner not reclaimed: ' + result
                    if mode == 'bios' and model == 'e1000':
                        silent.set()
                        launch = execute('ping -c 1 -W 5 10.0.2.2 &')
                        job = re.search(r'\[(\d+)\]', launch)
                        assert job, launch
                        execute(f'kill %{job[1]} STOP')
                        time.sleep(12)
                        execute(f'kill %{job[1]} CONT')
                        silent.clear()
                        result = execute('ping -c 1 10.0.2.2')
                        assert '1 probes, 1 replies' in result, 'STOP/CONT lease recovery failed: ' + result
                assert not peer_errors, peer_errors
            finally:
                stop.set()
                if uart is not None: uart.close()
                if proc is not None:
                    proc.terminate()
                    try: proc.wait(timeout=3)
                    except subprocess.TimeoutExpired: proc.kill(); proc.wait()
                if thread.is_alive(): thread.join(timeout=2)
                for path in (log, pcap, log.with_suffix('.stderr')):
                    if path.exists(): shutil.copyfile(path, saved_log.with_suffix(path.suffix))
        capture = packets(pcap)
        probes = [f for f in capture if f[6:12] == MAC and f[12:14] == b'\x08\0' and f[34] == 8]
        assert len(probes) >= 4
        for frame in probes:
            src, dst, body = decode(frame)
            assert src == GUEST_IP and dst == GATEWAY and len(body[8:]) == 32
        if backend == 'socket': assert received_echo[-1] in capture
        assert 'PANIC' not in text() and '[FATAL]' not in text()
        print(f'[PASS] {label}: Ring 3 ping 4/4, independent pcap checksum/length audit, '
              f'{"injected odd echo + timeout recovery" if backend == "socket" else "SLIRP gateway"}', flush=True)


def main():
    with tempfile.TemporaryDirectory(prefix='fortress-net-icmp-') as name:
        tmp = Path(name); iso = build_iso(tmp, 'net_test=arp net_test=icmp')
        for mode in ('bios', 'uefi'):
            for model in ('e1000', 'e1000e'):
                for backend in ('user', 'socket'): run(mode, model, backend, iso, tmp)
    print('All 8 Phase 4a QEMU cases PASS; no physical ICMP claim.', flush=True)


if __name__ == '__main__': main()
