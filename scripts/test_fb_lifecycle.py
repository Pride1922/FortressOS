#!/usr/bin/env python3
"""Private ISO-only graphics ownership/lifecycle acceptance, no data disks."""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path
from test_compositor import Guest, ROOT
from test_nmi_transitions import QMP


def rejected_frees(guest, kernel):
    # Read the real PMM counter without adding a public diagnostic ABI. All
    # vCPUs are stopped for the read; kernel mappings are shared by every CR3.
    rows = subprocess.check_output(['nm', '-an', str(kernel)], text=True).splitlines()
    matches = [int(row.split()[0], 16) for row in rows if len(row.split()) == 3 and row.split()[2] == 'rejected_frees']
    assert len(matches) == 1, 'PMM counter symbol is ambiguous/missing'
    guest.qmp.execute('stop')
    try:
        result = guest.qmp.execute('human-monitor-command', {'command-line': f'x /1gx 0x{matches[0]:x}'})
        value = re.search(r':\s*(0x[0-9a-fA-F]+)', result)
        assert value, f'Cannot read real PMM rejected-free counter: {result}'
        return int(value[1], 16)
    finally:
        guest.qmp.execute('cont')


def make_iso(tmp, source=ROOT, probes=None):
    root = tmp / 'iso'
    shutil.copytree(source / 'build/iso_root', root)
    archive = tmp / 'initramfs.tar'
    with tarfile.open(source / 'bin/initramfs.tar') as src, tarfile.open(archive, 'w', format=tarfile.USTAR_FORMAT) as dst:
        for member in src.getmembers():
            dst.addfile(member, src.extractfile(member) if member.isfile() else None)
        for name, path in (probes or {'fb-probe': source / 'build/fb-probe.elf'}).items():
            dst.add(path, arcname=f'bin/{name}')
    for target in [root / 'boot/initramfs.tar', root / 'initramfs.tar']:
        shutil.copyfile(archive, target)
    iso = tmp / 'fb.iso'
    subprocess.run(['xorriso', '-as', 'mkisofs', '-b', 'boot/limine/limine-bios-cd.bin',
                    '-no-emul-boot', '-boot-load-size', '4', '-boot-info-table',
                    '--efi-boot', 'boot/limine/limine-uefi-cd.bin', '-efi-boot-part',
                    '--efi-boot-image', '--protective-msdos-label', str(root), '-o', str(iso)],
                   check=True, timeout=60, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run([str(source / 'limine/limine'), 'bios-install', str(iso)],
                   check=True, timeout=30, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return iso


def run(tmp, iso, firmware, smp, ownership):
    mode = 'clients' if ownership else 'check'
    guest = Guest(firmware, smp, tmp, iso)
    kernel = tmp / 'iso/boot/fortress.elf'
    try:
        guest.wait(lambda s: s.endswith('FortressOS login: '), timeout=60)
        guest.qmp = QMP(tmp / 'qmp.sock')
        guest.send('operator')
        guest.wait(lambda s: s.endswith('$ '), timeout=20)
        rejected_before = rejected_frees(guest, kernel)
        start = len(guest.data)
        guest.send(f'sudo /bin/fb-probe {mode}')
        expected = 'FB_CLIENTS_PASS' if ownership else 'FB_LIFECYCLE_PASS'
        result = guest.wait(lambda s: expected in s or '[FB FAIL]' in s, start, timeout=90)
        assert '[FB FAIL]' not in result, result[-3000:]
        guest.wait(lambda s: s.endswith('$ '), start, timeout=20)
        if ownership:
            assert '[compositor] Display is owned by another process' in result
            assert '[CONSOLE] Display owner exited, restoring text mode' in result
        else:
            assert 'FB_CONTENDER_PASS' in result
            assert 'FB_SMP_RACE_PASS' in result
            assert 'FB_CONSOLE_SERIAL_ONLY' in result
            baseline = re.search(r'FB_BASE free=(\d+) tables=(\d+) deferred=(\d+)', result)
            final = re.search(r'FB_FINAL free=(\d+) tables=(\d+) deferred=(\d+)', result)
            assert baseline and final, result[-3000:]
            checkpoints = re.findall(r'FB_CHECKPOINT free=(\d+) tables=(\d+) deferred=(\d+)', result)
            assert checkpoints == [baseline.groups()]*4, ('intermediate memory drift', checkpoints)
            assert baseline.groups() == final.groups(), ('memory accounting drift', baseline.groups(), final.groups())
        assert expected in result
        rejected_after = rejected_frees(guest, kernel)
        (ROOT / 'build' / f'fb-counters-{mode}-{firmware}-{smp}.json').write_text(json.dumps({
            'rejected_frees_before': rejected_before, 'rejected_frees_after': rejected_after,
            'kernel_sha256': hashlib.sha256(kernel.read_bytes()).hexdigest(),
            'initramfs_sha256': hashlib.sha256((tmp / 'initramfs.tar').read_bytes()).hexdigest(),
            'firmware': firmware, 'smp': smp, 'mode': mode}, indent=2))
        assert rejected_after == rejected_before, ('invalid PMM free', rejected_before, rejected_after)
        # Recovery must leave a functioning text console and command path.
        command = guest.command('echo FB_SHELL_RECOVERED')
        assert 'FB_SHELL_RECOVERED' in command
        print(f'PASS {mode} {firmware} SMP={smp} rejected_frees={rejected_before}->{rejected_after}', flush=True)
    finally:
        try:
            (ROOT / 'build' / f'fb-{mode}-{firmware}-{smp}.log').write_text(guest.text())
        finally:
            guest.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--ownership', action='store_true')
    parser.add_argument('--teardown', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='fortress-fb-') as folder:
        tmp = Path(folder)
        iso = make_iso(tmp)
        for firmware in ['bios', 'uefi']:
            for smp in [1, 4]:
                run(tmp, iso, firmware, smp, args.ownership)


if __name__ == '__main__':
    main()
