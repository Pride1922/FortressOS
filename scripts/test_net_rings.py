#!/usr/bin/env python3
"""NET 2a: real descriptor DMA, pcap TX audit, loopback UDP raw RX injection.
Disposable ISO/OVMF vars, SMP=1, exact argv whitelist, no data disks.
UDP carries raw Ethernet bytes; it adds no guest protocol implementation.
"""
import re
import shutil
import socket
import struct
import subprocess
import tempfile
import time
from pathlib import Path
from test_net_pci import REPO, CODE, VARS


def build_iso(tmp):
    root = tmp / 'root'
    shutil.copytree(REPO / 'build/iso_root', root)
    config = ('timeout: 0\n/FortressOS NET Rings Test\n    protocol: limine\n'
              '    kernel_path: boot():/boot/fortress.elf\n'
              '    module_path: boot():/boot/initramfs.tar\n'
              '    kernel_cmdline: net_test=rings\n')
    for path in (root / 'limine.conf', root / 'boot/limine.conf', root / 'boot/limine/limine.conf'):
        path.write_text(config)
    iso = tmp / 'rings.iso'
    subprocess.run(['xorriso', '-as', 'mkisofs', '-b', 'boot/limine/limine-bios-cd.bin',
                    '-no-emul-boot', '-boot-load-size', '4', '-boot-info-table',
                    '--efi-boot', 'boot/limine/limine-uefi-cd.bin', '-efi-boot-part',
                    '--efi-boot-image', '--protective-msdos-label', str(root), '-o', str(iso)],
                   check=True, timeout=60, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run([str(REPO / 'limine/limine'), 'bios-install', str(iso)],
                   check=True, timeout=30, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return iso


def command(mode, model, iso, variables, log, pcap, host_port, guest_port):
    cmd = ['qemu-system-x86_64', '-M', 'q35', '-m', '2G', '-accel', 'tcg',
           '-smp', '1', '-display', 'none', '-monitor', 'none', '-no-reboot',
           '-boot', 'd', '-cdrom', str(iso), '-serial', f'file:{log}',
           '-netdev', f'socket,id=net0,udp=127.0.0.1:{host_port},localaddr=127.0.0.1:{guest_port}',
           '-device', f'{model},netdev=net0,mac=52:54:00:12:34:56',
           '-object', f'filter-dump,id=dump0,netdev=net0,file={pcap}']
    if mode == 'uefi':
        cmd += ['-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
                '-drive', f'if=pflash,format=raw,unit=1,file={variables}']
    return cmd


def preflight(cmd, *args):
    assert args[0] in ('bios', 'uefi') and args[1] in ('e1000', 'e1000e')
    assert cmd == command(*args), f'unexpected QEMU argv: {cmd}'


def packets(path):
    data = path.read_bytes()
    assert len(data) >= 24, 'missing pcap header'
    assert data[:4] in (b'\xd4\xc3\xb2\xa1', b'\xa1\xb2\xc3\xd4'), 'unknown pcap magic'
    endian = '<' if data[:4] == b'\xd4\xc3\xb2\xa1' else '>'
    assert struct.unpack_from(endian + 'I', data, 20)[0] == 1, 'pcap must be Ethernet'
    offset, result = 24, []
    while offset < len(data):
        assert offset + 16 <= len(data), 'truncated pcap record'
        _, _, size, original = struct.unpack_from(endian + '4I', data, offset)
        offset += 16
        assert size == original and offset + size <= len(data), 'truncated packet'
        result.append(data[offset:offset + size])
        offset += size
    return result


def run(mode, model, iso, tmp):
    label = f'{mode}-{model}'
    log = REPO / 'build' / f'test-net-rings-{label}.log'
    pcap = REPO / 'build' / f'test-net-rings-{label}.pcap'
    log.write_text('')
    variables = tmp / f'{label}-vars.fd'
    if mode == 'uefi':
        shutil.copyfile(VARS, variables)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as host:
        host.bind(('127.0.0.1', 0))
        host.settimeout(.05)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
            reservation.bind(('127.0.0.1', 0))
            guest_port = reservation.getsockname()[1]
        args = (mode, model, iso, variables, log, pcap, host.getsockname()[1], guest_port)
        cmd = command(*args)
        preflight(cmd, *args)
        for extra in (['-drive', 'file=unsafe.img'], ['-device', 'nvme'],
                      ['-blockdev', 'driver=file,filename=unsafe.img'],
                      ['-device', 'usb-storage,drive=x'], ['-netdev', 'user,id=extra'],
                      ['-object', 'filter-dump,id=extra,netdev=net0,file=bad.pcap']):
            try:
                preflight(cmd + extra, *args)
            except AssertionError:
                pass
            else:
                raise AssertionError(f'preflight accepted {extra}')
        tx = (b'\xff' * 6 + bytes.fromhex('52540012345688b5') +
              b'FORTRESS-NET-2A-TX' + b'\xa5' * 28)
        rx = tx[:14] + b'FORTRESS-NET-2A-RX' + tx[32:]
        assert len(tx) == len(rx) == 60
        received_tx = False
        injected = False
        with log.with_suffix('.stderr').open('wb') as stderr:
            proc = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=stderr)
            try:
                deadline = time.monotonic() + 45
                while time.monotonic() < deadline:
                    assert proc.poll() is None, f'QEMU exited: {log.with_suffix(".stderr").read_text()}'
                    try:
                        frame, _ = host.recvfrom(65536)
                        if frame == tx:
                            received_tx = True
                    except socket.timeout:
                        pass
                    text = log.read_text(errors='replace')
                    if received_tx and '[NET 2a] RX waiting' in text and not injected:
                        host.sendto(rx, ('127.0.0.1', guest_port))
                        injected = True
                    if '[BOOT] Interactive shell ready.' in text:
                        break
                else:
                    raise AssertionError(f'boot timeout: {text[-3000:]}')
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
        assert received_tx, 'host did not receive exact TX bytes'
        assert '[NET 2a] TX PASS' in text and '[NET 2a] RX PASS' in text, text[-3000:]
        assert tx in packets(pcap), 'pcap missing exact 60-byte TX frame'
        assert rx in packets(pcap), 'pcap missing exact injected RX frame'
        assert 'PANIC' not in text and '[NET 2a] RX timeout' not in text
        print(f'[PASS] {label} SMP=1: TX DD + pcap 60/60 byte match; RX injected 60/60 byte match + recycle; shell ready', flush=True)
        for line in text.splitlines():
            if '[NET 2a]' in line:
                print('  ' + line, flush=True)


def main():
    with tempfile.TemporaryDirectory(prefix='fortress-net-rings-') as name:
        tmp = Path(name)
        iso = build_iso(tmp)
        for mode in ('bios', 'uefi'):
            for model in ('e1000', 'e1000e'):
                run(mode, model, iso, tmp)
    print('All 4 NET Phase 2a QEMU cases PASS. RX coverage: one injected raw frame per case; no worker/protocol/hardware claim.', flush=True)


if __name__ == '__main__':
    main()
