#!/usr/bin/env python3
"""Strict saved-argv audit and adversarial exclusion selftests; no QEMU launch."""
import argparse
import json
from pathlib import Path
import tempfile
import test_net_installed as baseline


def audit(argv, out, case):
    firmware, smp, boot = case.split('-')
    disk = out / f'{firmware}-{smp}.img'
    expected = ['qemu-system-x86_64', '-accel', 'tcg', '-M', 'q35', '-m', '512M',
                '-smp', smp[3:], '-display', 'none', '-monitor', 'none', '-no-reboot',
                '-serial', 'stdio', '-drive', f'file={disk},if=none,id=target,format=raw',
                '-device', 'nvme,drive=target,serial=NETINSTALLED,bootindex=1',
                '-netdev', 'user,id=net0', '-device', 'e1000,netdev=net0']
    assert firmware in ('bios', 'uefi') and smp in ('smp1', 'smp4', 'smp8')
    assert boot in ('boot0', 'boot1')
    assert disk.is_file() and disk.resolve().parent == out.resolve()
    if firmware == 'uefi':
        vars_path = out / (case + '.vars.fd')
        assert vars_path.is_file() and vars_path.resolve().parent == out.resolve()
        expected += ['-drive', 'if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
                     '-drive', f'if=pflash,format=raw,unit=1,file={vars_path}']
    assert argv == expected, 'Unexpected device, disk, firmware or QEMU option'
    return True


def main():
    parser = argparse.ArgumentParser(); parser.add_argument('campaign', type=Path)
    args = parser.parse_args(); out = args.campaign.resolve()
    manifest = json.loads((out / 'manifest.json').read_text())
    count = 0
    for case in manifest['cases']:
        label = case['case']; argv = json.loads((out / (label + '.argv.json')).read_text())
        assert audit(argv, out, label)
        # Reject attachments through every alternate QEMU storage mechanism.
        hostile = [argv + ['-drive', 'file=/dev/sda,format=raw'],
                   argv + ['-blockdev', 'driver=host_device,filename=/dev/sda'],
                   argv + ['-cdrom', '/tmp/shared.iso'],
                   argv + ['-device', 'usb-storage,drive=shared']]
        changed = argv.copy(); changed[changed.index('-drive') + 1] = 'file=/tmp/shared.img,format=raw'
        hostile.append(changed)
        for candidate in hostile:
            try:
                audit(candidate, out, label)
            except AssertionError:
                count += 1
            else:
                raise AssertionError('Unauthorized storage accepted')
    # Exercise the baseline's prelaunch outside-directory rejection, without
    # letting a real process launch even if the guard regresses.
    with tempfile.TemporaryDirectory() as scratch:
        disk = Path(scratch) / 'outside.img'; disk.write_bytes(b'fixture')
        original = baseline.subprocess.Popen
        def forbidden(*args, **kwargs):
            raise RuntimeError('Prelaunch disk exclusion did not reject input')
        baseline.subprocess.Popen = forbidden
        try:
            try:
                baseline.boot(out, disk, 'bios', 1, 0, 'unused', 1)
            except AssertionError:
                count += 1
            else:
                raise AssertionError('Outside fixture directory accepted')
        finally:
            baseline.subprocess.Popen = original
    result = {'result': 'PASS', 'argv_audits': len(manifest['cases']),
              'negative_exclusions': count, 'no_process_launched_by_selftest': True}
    (out / 'exclusions.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
