#!/usr/bin/env python3
"""NET 3: user-net gateway ARP and UDP socket-backend RX injection.
Disposable ISO/OVMF vars, BIOS/UEFI, e1000/e1000e, SMP=1, no data disks.
Capture is independently audited for outbound frames; socket injection is
explicit RX evidence, not filter-dump injection or physical wire evidence.
"""
import shutil
import socket
import struct
import subprocess
import tempfile
import time
from pathlib import Path
from test_net_pci import REPO, CODE, VARS
from test_net_rings import packets

MAC = bytes.fromhex('525400123456')
PEER = bytes.fromhex('020304050607')
GUEST_IP = bytes([10, 0, 2, 15])
GATEWAY = bytes([10, 0, 2, 2])


def arp_frame(reply, sender_mac, sender_ip, target_mac, target_ip):
    dest = target_mac if reply else b'\xff' * 6
    arp = struct.pack('!HHBBH', 1, 0x800, 6, 4, 2 if reply else 1)
    arp += sender_mac + sender_ip + target_mac + target_ip
    return dest + sender_mac + b'\x08\x06' + arp + b'\0' * 18


def build_iso(tmp, cmdline='net_test=arp'):
    root = tmp / 'root'
    shutil.copytree(REPO / 'build/iso_root', root)
    config = ('timeout: 0\n/FortressOS NET ARP Test\n    protocol: limine\n'
              '    kernel_path: boot():/boot/fortress.elf\n'
              '    module_path: boot():/boot/initramfs.tar\n'
              f'    kernel_cmdline: {cmdline}\n')
    for path in (root / 'limine.conf', root / 'boot/limine.conf', root / 'boot/limine/limine.conf'):
        path.write_text(config)
    iso = tmp / 'arp.iso'
    subprocess.run(['xorriso', '-as', 'mkisofs', '-b', 'boot/limine/limine-bios-cd.bin',
                    '-no-emul-boot', '-boot-load-size', '4', '-boot-info-table',
                    '--efi-boot', 'boot/limine/limine-uefi-cd.bin', '-efi-boot-part',
                    '--efi-boot-image', '--protective-msdos-label', str(root), '-o', str(iso)],
                   check=True, timeout=60, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run([str(REPO / 'limine/limine'), 'bios-install', str(iso)],
                   check=True, timeout=30, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return iso


def command(mode, model, backend, iso, variables, log, pcap, host_port, guest_port):
    assert mode in ('bios', 'uefi') and model in ('e1000', 'e1000e')
    assert backend in ('user', 'socket')
    net = 'user,id=net0' if backend == 'user' else (
        f'socket,id=net0,udp=127.0.0.1:{host_port},localaddr=127.0.0.1:{guest_port}')
    cmd = ['qemu-system-x86_64', '-M', 'q35', '-m', '2G', '-accel', 'tcg',
           '-smp', '1', '-display', 'none', '-monitor', 'none', '-no-reboot',
           '-boot', 'd', '-cdrom', str(iso), '-serial', f'file:{log}',
           '-netdev', net, '-device', f'{model},netdev=net0,mac=52:54:00:12:34:56',
           '-object', f'filter-dump,id=dump0,netdev=net0,file={pcap}']
    if mode == 'uefi':
        cmd += ['-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
                '-drive', f'if=pflash,format=raw,unit=1,file={variables}']
    return cmd


def preflight(cmd, args):
    assert cmd == command(*args), f'unexpected QEMU argv: {cmd}'


def run(mode, model, backend, iso, tmp):
    label = f'{mode}-{model}-{backend}'
    log = REPO / 'build' / f'test-net-eth-{label}.log'
    pcap = log.with_suffix('.pcap')
    log.write_text('')
    variables = tmp / f'{label}-vars.fd'
    if mode == 'uefi':
        shutil.copyfile(VARS, variables)
    request = arp_frame(False, MAC, GUEST_IP, b'\0' * 6, GATEWAY)
    injection = arp_frame(False, PEER, GATEWAY, b'\0' * 6, GUEST_IP)
    expected_reply = arp_frame(True, MAC, GUEST_IP, PEER, GATEWAY)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as host:
        host.bind(('127.0.0.1', 0))
        host.settimeout(.03)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
            reservation.bind(('127.0.0.1', 0))
            guest_port = reservation.getsockname()[1]
        args = (mode, model, backend, iso, variables, log, pcap, host.getsockname()[1], guest_port)
        cmd = command(*args)
        preflight(cmd, args)
        for extra in (['-drive', 'file=unsafe.img'], ['-device', 'nvme'],
                      ['-blockdev', 'driver=file,filename=unsafe.img'],
                      ['-device', 'usb-storage,drive=x'], ['-netdev', 'user,id=extra']):
            try:
                preflight(cmd + extra, args)
            except AssertionError:
                pass
            else:
                raise AssertionError(f'preflight accepted {extra}')
        received_reply = False
        injected = False
        text = ''
        with log.with_suffix('.stderr').open('wb') as stderr:
            proc = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=stderr)
            try:
                deadline = time.monotonic() + 45
                while time.monotonic() < deadline:
                    assert proc.poll() is None, log.with_suffix('.stderr').read_text()
                    if backend == 'socket':
                        try:
                            frame, _ = host.recvfrom(65536)
                            if frame == request:
                                # Socket backend has no SLIRP gateway: explicitly emulate its reply.
                                host.sendto(arp_frame(True, PEER, GATEWAY, MAC, GUEST_IP),
                                            ('127.0.0.1', guest_port))
                            if frame == expected_reply:
                                received_reply = True
                        except socket.timeout:
                            pass
                    else:
                        time.sleep(.03)
                    text = log.read_text(errors='replace')
                    if '[NET 3] Gateway ARP resolved' in text and '[BOOT] Interactive shell ready.' in text:
                        if backend == 'socket' and not injected:
                            # Inject after idle ticks: proves the sleeping worker resumes to poll RX.
                            time.sleep(.15)
                            host.sendto(injection, ('127.0.0.1', guest_port))
                            injected = True
                            continue
                        if backend == 'user' or received_reply:
                            # Keep the sleeping worker alive across additional timer ticks.
                            time.sleep(.3)
                            break
                else:
                    raise AssertionError(f'ARP/boot timeout: {text[-4000:]}')
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
        text = log.read_text(errors='replace')
        assert '[NET 3] BSP ingress worker started' in text
        assert '[NET 3] Gateway ARP request submitted' in text
        assert '[NET 3] Gateway ARP resolved' in text
        assert 'FortressOS shell (Ring 3)' in text and 'fortress> ' in text, 'missing real shell prompt'
        assert 'PANIC' not in text and '[FATAL]' not in text and '[NET 3] Gateway ARP timeout' not in text
        captured = packets(pcap)
        assert request in captured, 'pcap missing exact outbound ARP request'
        if backend == 'socket':
            assert received_reply and expected_reply in captured, 'missing guest ARP reply'
        print(f'[PASS] {label} SMP=1: gateway resolved, 60-byte TX pcap match, '
              f'{"injected request + guest reply" if backend == "socket" else "SLIRP gateway reply"}, shell ready', flush=True)


def main():
    with tempfile.TemporaryDirectory(prefix='fortress-net-eth-') as name:
        tmp = Path(name)
        iso = build_iso(tmp)
        for mode in ('bios', 'uefi'):
            for model in ('e1000', 'e1000e'):
                for backend in ('user', 'socket'):
                    run(mode, model, backend, iso, tmp)
    print('All 8 NET Phase 3 QEMU cases PASS. No physical hardware or idle CPU percentage claim.', flush=True)


if __name__ == '__main__':
    main()
