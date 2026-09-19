#!/usr/bin/env python3
"""9G.1a PCI-only discovery: BIOS/UEFI, present/absent xHCI, no data disk.

Boot media is the ISO; no USB transfers or persistence are claimed. Firmware
code and a disposable vars copy are the only permitted -drive backends.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
import argparse
from test_nmi_transitions import QMP

REPO = Path(__file__).resolve().parent.parent
CODE = Path('/usr/share/OVMF/OVMF_CODE_4M.fd')
VARS = Path('/usr/share/OVMF/OVMF_VARS_4M.fd')


def run(firmware, present, mode='discovery'):
    name = f'usb-{mode}-{firmware}-{"present" if present else "absent"}'
    log = REPO / 'build' / f'{name}.log'
    # Never accept a prior run's prompt before QEMU opens its serial logfile.
    log.write_text('')
    with tempfile.TemporaryDirectory(prefix='fortress-usb-discovery-') as tmp:
        cmd = ['qemu-system-x86_64', '-M', 'q35', '-m', '2G', '-accel', 'tcg',
               '-smp', '1', '-display', 'none', '-monitor', 'none', '-no-reboot',
               '-serial', f'file:{log}', '-boot', 'd', '-cdrom', 'bin/fortress.iso']
        firmware_drives = []
        if firmware == 'uefi':
            assert CODE.is_file() and VARS.is_file(), 'Paired OVMF 4M firmware required'
            variables = Path(tmp) / 'vars.fd'
            shutil.copyfile(VARS, variables)
            firmware_drives = [
                f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
                f'if=pflash,format=raw,unit=1,file={variables}',
            ]
            for drive in firmware_drives:
                cmd += ['-drive', drive]
        if present:
            if mode in ('descriptors', 'block'):
                cmd += ['-device', 'qemu-xhci,id=xhci,p2=4,p3=0']
                usb_disk = Path(tmp) / 'usb_storage.img'
                if mode == 'block' and (REPO / 'bin' / 'fortress.img').exists():
                    usb_disk.write_bytes((REPO / 'bin' / 'fortress.img').read_bytes())
                else:
                    content = bytearray(1024 * 1024)
                    content[510] = 0x55
                    content[511] = 0xAA
                    usb_disk.write_bytes(content)
                cmd += ['-drive', f'if=none,id=usbdrive,format=raw,file={usb_disk}',
                        '-device', 'usb-storage,drive=usbdrive']
            else:
                cmd += ['-device', 'qemu-xhci,id=xhci']
        if mode in ('reset', 'rings', 'ports', 'descriptors', 'block'):
            cmd += ['-qmp', f'unix:{tmp}/qmp,server=on,wait=off']

        # Assert final argv, not just inputs: no NVMe/USB data fixture is needed
        # for PCI discovery. ISO boot is the sole non-firmware storage backend.
        permitted_drives = list(firmware_drives)
        if mode in ('descriptors', 'block') and present:
            permitted_drives.append(f'if=none,id=usbdrive,format=raw,file={usb_disk}')
        assert [cmd[i + 1] for i, arg in enumerate(cmd) if arg == '-drive'] == permitted_drives
        assert '-blockdev' not in cmd and '-hda' not in cmd and '-hdb' not in cmd
        expected_devices = []
        if present:
            if mode in ('descriptors', 'block'):
                expected_devices.append('qemu-xhci,id=xhci,p2=4,p3=0')
                expected_devices.append('usb-storage,drive=usbdrive')
            else:
                expected_devices.append('qemu-xhci,id=xhci')
        assert [cmd[i + 1] for i, arg in enumerate(cmd) if arg == '-device'] == expected_devices
        with (REPO / 'build' / f'{name}.stderr').open('w') as err:
            child = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=err)
            qmp = None
            try:
                if mode in ('reset', 'rings', 'ports', 'descriptors', 'block'):
                    qmp = QMP(Path(tmp) / 'qmp')
                deadline = time.monotonic() + 90
                while time.monotonic() < deadline:
                    assert child.poll() is None
                    time.sleep(0.5)
                    if log.exists():
                        output = log.read_text(errors='replace')
                        if 'FortressOS shell (Ring 3)' in output and 'fortress> ' in output:
                            break
                else:
                    raise AssertionError(f'{name}: shell prompt not reached within timeout')
                output = log.read_text(errors='replace')
                if present:
                    assert 'First xHCI controller:' in output
                    assert 'Discovery complete; BAR extent/MMIO unverified' in output
                    assert 'No xHCI controller found' not in output
                else:
                    assert 'No xHCI controller found; continuing without USB storage' in output
                    assert 'First xHCI controller:' not in output
                if mode in ('reset', 'rings', 'ports', 'descriptors', 'block'):
                    if present:
                        assert '[USB 9G.1b] PASS: reset complete; halted, CNR=0' in output
                        assert '[USB 9G.1b] Unavailable:' not in output
                        assert 'bus mastering disabled' in output
                        if mode in ('rings', 'ports', 'descriptors', 'block'):
                            assert '[USB 9G.1c] PASS: No-Op command completed' in output
                            assert '[USB 9G.1c] FAIL:' not in output
                        if mode in ('ports', 'descriptors', 'block'):
                            assert '[USB 9G.1d] Root ports:' in output
                            assert '[USB 9G.1d] PASS:' in output
                        if mode in ('descriptors', 'block'):
                            assert '[USB 9G.1e] PASS: Device addressed on Slot' in output
                            assert '[USB 9G.1e] PASS: BOT Mass Storage device configured and ready for 9G.2 block I/O' in output
                            assert '[USB 9G.1e] FAIL:' not in output
                        if mode == 'block':
                            assert '[USB 9G.2] Bulk endpoints configured' in output
                            assert '[USB 9G.2] SCSI INQUIRY:' in output
                            assert '[USB 9G.2] SCSI Capacity:' in output
                            assert '[USB 9G.2] PASS: Registered block device "sda"' in output
                            assert '[USB 9G.2] PASS: Sector 0 read verified' in output
                    else:
                        assert '[USB 9G.1b] No xHCI controller; skipped' in output
                    # Real PS/2 delivery after controller reset/rings/ports/descriptors; not just a banner.
                    start = len(log.read_text(errors='replace'))
                    for char in 'echo resetok\n':
                        code = {' ': 'spc', '\n': 'ret'}.get(char, char)
                        qmp.execute('input-send-event', {'events': [
                            {'type': 'key', 'data': {'down': down,
                             'key': {'type': 'qcode', 'data': code}}}
                            for down in (True, False)]})
                        time.sleep(0.03)
                    deadline = time.monotonic() + 10
                    while time.monotonic() < deadline:
                        reply = log.read_text(errors='replace')[start:].replace('\r', '')
                        if '\nresetok\n' in reply and 'fortress> ' in reply:
                            break
                        assert child.poll() is None
                        time.sleep(0.05)
                    else:
                        raise AssertionError(f'{name}: post-reset PS/2 echo failed')
                    qmp.execute('screendump', {'filename': str(REPO / 'build' / f'{name}.png'),
                                              'format': 'png'})
                print(f'PASS {name}: PCI result and interactive shell; {log}', flush=True)
            finally:
                if qmp:
                    qmp.stream.close()
                    qmp.sock.close()
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--reset', action='store_true')
    parser.add_argument('--rings', action='store_true')
    parser.add_argument('--ports', action='store_true')
    parser.add_argument('--descriptors', action='store_true')
    parser.add_argument('--block', action='store_true')
    args = parser.parse_args()
    mode = 'block' if args.block else ('descriptors' if args.descriptors else ('ports' if args.ports else ('rings' if args.rings else ('reset' if args.reset else 'discovery'))))
    for firmware in ('bios', 'uefi'):
        for present in (False, True):
            run(firmware, present, mode)
