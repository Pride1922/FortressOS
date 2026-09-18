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

REPO = Path(__file__).resolve().parent.parent
CODE = Path('/usr/share/OVMF/OVMF_CODE_4M.fd')
VARS = Path('/usr/share/OVMF/OVMF_VARS_4M.fd')


def run(firmware, present):
    name = f'usb-discovery-{firmware}-{"present" if present else "absent"}'
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
            cmd += ['-device', 'qemu-xhci,id=xhci']

        # Assert final argv, not just inputs: no NVMe/USB data fixture is needed
        # for PCI discovery. ISO boot is the sole non-firmware storage backend.
        assert [cmd[i + 1] for i, arg in enumerate(cmd) if arg == '-drive'] == firmware_drives
        assert '-blockdev' not in cmd and '-hda' not in cmd and '-hdb' not in cmd
        assert [cmd[i + 1] for i, arg in enumerate(cmd) if arg == '-device'] == (
            ['qemu-xhci,id=xhci'] if present else [])
        with (REPO / 'build' / f'{name}.stderr').open('w') as err:
            child = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=err)
            try:
                deadline = time.monotonic() + 90
                while time.monotonic() < deadline:
                    output = log.read_text(errors='replace') if log.exists() else ''
                    if 'fortress> ' in output:
                        break
                    assert child.poll() is None, f'{name}: QEMU exited; see stderr/log'
                    time.sleep(0.1)
                else:
                    raise AssertionError(f'{name}: shell timeout; see {log}')
                assert '[FAIL]' not in output, f'{name}: boot failure; see {log}'
                assert '[SKIP] No NVMe controller found' in output
                assert '[USB 9G.1a] PCI discovery only' in output
                if present:
                    assert re.search(r'First xHCI controller: .*vendor=0x\w+ device=0x\w+', output)
                    match = re.search(r'\[USB 9G.1a\] BAR0=(0x[0-9A-Fa-f]+) memory(?:32|64) prefetch=(?:yes|no)', output)
                    assert match and int(match[1], 16) != 0, f'{name}: missing valid BAR'
                    assert 'Discovery complete; BAR extent/MMIO unverified' in output
                    assert 'No xHCI controller found' not in output
                else:
                    assert 'No xHCI controller found; continuing without USB storage' in output
                    assert 'First xHCI controller:' not in output
                print(f'PASS {name}: PCI result and interactive shell; {log}', flush=True)
            finally:
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()


if __name__ == '__main__':
    for firmware in ('bios', 'uefi'):
        for present in (False, True):
            run(firmware, present)
