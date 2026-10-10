"""BIOS/UEFI production journaled EXT4 bounded cache reclamation and reboot."""
import datetime
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from test_memory_ext4 import ROOT, digest, journal_info
from test_usb_persistence import run_qemu_session, send_command


def offline(disk, output):
    with tempfile.TemporaryDirectory(prefix='fortress-churn-audit-') as tmp:
        part = Path(tmp) / 'data.ext4'
        with disk.open('rb') as stream:
            stream.seek(133120 * 512)
            data = stream.read(131072 * 512)
        assert len(data) == 131072 * 512
        part.write_bytes(data)
        result = subprocess.run(['e2fsck', '-fn', str(part)], capture_output=True, text=True, timeout=60)
        output.write_text(result.stdout + result.stderr)
        assert result.returncode == 0
        result = subprocess.run(['debugfs', '-R', 'stat /memory-churn.bin', str(part)],
                                capture_output=True, text=True, check=True, timeout=30)
        output.with_suffix('.absent.txt').write_text(result.stdout + result.stderr)
        assert 'File not found' in result.stderr


def main():
    out = ROOT / 'build' / ('memory-churn-qemu-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ'))
    out.mkdir(parents=True)
    source = ROOT / 'bin/fortress.img'
    protected = ROOT / 'docs/plans/PERMISSIONS_PLAN.md'
    before = {str(p): digest(p) for p in (source, protected)}
    records = []
    try:
        for firmware in ('bios', 'uefi'):
            disk = out / (firmware + '.img')
            shutil.copyfile(source, disk)
            journal_info(disk, out / (firmware + '-before.features.txt'))

            def exhausted(qmp, child, log):
                text = log.read_text(errors='replace')
                assert '[USB E4-B] Selected filesystem: ext4 journaled' in text
                assert re.search(r'\[MEMORY CHURN\] PASS rounds=1152 heap_used_delta=0 retained_blocks=0', text)
                assert '[MEMORY CHURN] existing uncached file read PASS' in text
                send_command(qmp, child, log, 'sync\n', 'Filesystem synced.')
                send_command(qmp, child, log, 'shutdown\n', 'Shutdown initiated')

            def recovered(qmp, child, log):
                text = log.read_text(errors='replace')
                assert '[USB E4-B] Selected filesystem: ext4 journaled' in text
                assert '[MEMORY CHURN]' not in text
                send_command(qmp, child, log, 'echo memory-ext4-persistence > /mnt/memory-check.txt\n', ' $ ')
                send_command(qmp, child, log, 'cat /mnt/memory-check.txt\n', 'memory-ext4-persistence')
                send_command(qmp, child, log, 'sync\n', 'Filesystem synced.')
                send_command(qmp, child, log, 'shutdown\n', 'Shutdown initiated')

            for boot, action in enumerate((exhausted, recovered), 1):
                log = out / f'{firmware}-boot{boot}.log'
                run_qemu_session(firmware, disk, log, action, 'memory-churn', cpus=4, memory_churn=(boot == 1))
                offline(disk, out / f'{firmware}-boot{boot}.fsck.txt')
                journal_info(disk, out / f'{firmware}-boot{boot}.features.txt',
                             expected_file=True if boot == 2 else None)
            records.append({'firmware': firmware, 'cpus': 4, 'status': 'PASS'})
            print(f'PASS EXT4 memory churn {firmware} SMP=4, reclamation and reboot recovery', flush=True)
    finally:
        after = {str(p): digest(p) for p in (source, protected)}
        status = 'PASS' if len(records) == 2 and before == after else 'FAIL'
        (out / 'result.json').write_text(json.dumps({'status': status, 'before': before,
            'after': after, 'cases': records, 'scope': 'Synchronous BSP kernel VFS churn, Ring 3 recovery; no cross-core namespace race or hardware claim'}, indent=2) + '\n')
        print(f'EXT4 QEMU memory churn {status}: {out}', flush=True)
        assert before == after


if __name__ == '__main__':
    main()
