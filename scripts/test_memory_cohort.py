"""Concurrent process peaks and fragmented PMM headroom; ISO-only QEMU diagnostics."""
import datetime
import json
from pathlib import Path
import re
import shutil
import subprocess
import time
from test_smp_memory_boot import make_iso, CODE, VARS, REPO
from test_memory_ext4 import digest


def main():
    out = REPO / 'build' / ('memory-cohort-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ'))
    out.mkdir(parents=True)
    tracked = [REPO / 'bin/fortress.img', REPO / 'bin/fortress.elf', REPO / 'docs/plans/PERMISSIONS_PLAN.md']
    before = {str(p): digest(p) for p in tracked}
    record = {'status': 'FAIL', 'before': before, 'cases': []}
    try:
        with (out / 'fixture.log').open('w') as log:
            import contextlib
            with contextlib.redirect_stdout(log):
                iso = make_iso(out)
        for firmware, ram, enabled in [('bios', '256M', True), ('uefi', '256M', True),
                                     ('bios', '512M', True), ('uefi', '512M', True), ('bios', '256M', False)]:
            label = f'{firmware}-{ram}-' + ('cohort' if enabled else 'control')
            uart = out / (label + '.log')
            stderr = out / (label + '.stderr')
            command = ['qemu-system-x86_64', '-accel', 'tcg', '-M', 'q35', '-m', ram, '-smp', '1',
                       '-display', 'none', '-monitor', 'none', '-no-reboot', '-serial', f'file:{uart}',
                       '-boot', 'd', '-cdrom', str(iso)]
            drives = []
            if firmware == 'uefi':
                assert CODE.is_file() and VARS.is_file()
                variables = out / (label + '-vars.fd')
                shutil.copyfile(VARS, variables)
                drives = [f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
                          f'if=pflash,format=raw,unit=1,file={variables}']
                for drive in drives:
                    command += ['-drive', drive]
            if enabled:
                command += ['-fw_cfg', 'name=opt/fortress/memory_cohort_test,string=1']
            assert [command[i + 1] for i, a in enumerate(command) if a == '-drive'] == drives
            assert not any(a in command for a in ('-device', '-blockdev', '-hda', '-hdb'))
            case = {'firmware': firmware, 'ram': ram, 'enabled': enabled, 'argv': command, 'status': 'FAIL'}
            record['cases'].append(case)
            (out / (label + '.argv.json')).write_text(json.dumps(command, indent=2) + '\n')
            start = time.monotonic()
            with stderr.open('w') as err:
                child = subprocess.Popen(command, cwd=REPO, stdout=subprocess.DEVNULL, stderr=err)
                try:
                    while time.monotonic() - start < 180:
                        text = uart.read_text(errors='replace') if uart.exists() else ''
                        assert '[FAIL]' not in text, f'boot assertion: {uart}'
                        assert 'KERNEL PANIC' not in text, f'panic: {uart}'
                        if re.search(r'(?:fortress> |fortress:[^\r\n]* \$ )', text):
                            break
                        assert child.poll() is None, f'QEMU exited early: {stderr}'
                        time.sleep(0.1)
                    else:
                        raise TimeoutError(f'shell timeout: {uart}')
                finally:
                    if child.poll() is None:
                        child.terminate()
                    try:
                        child.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait(timeout=5)
            text = uart.read_text(errors='replace')
            if enabled:
                base_m = re.search(
                    r'\[MEMORY COHORT\] baseline heap_used=(\d+) heap_committed=(\d+) heap_free=(\d+) largest_payload=(\d+) pmm_free=(\d+) tables=(\d+) deferred=(\d+)',
                    text)
                cohort_matches = re.findall(
                    r'\[MEMORY COHORT\] cohort size=(\d+) pass=(\d+) peak_heap=(\d+) post_heap=(\d+) heap_committed=(\d+) heap_delta=(\d+) table_delta=(\d+) (overlap=PASS mixed_reap=PASS|repeat_stable=PASS)',
                    text)
                frag_m = re.search(
                    r'\[MEMORY COHORT\] fragmented_pmm held_pages=(\d+) scattered_pages=(\d+) contig2_fail=PASS contig4_fail=PASS single_page=PASS proc_spawn=PASS oom_spawn_fail=PASS contig_restore=PASS post_spawn=PASS',
                    text)
                rec_m = re.search(
                    r'\[MEMORY COHORT\] recovery exact_heap=PASS exact_tables=PASS zero_deferred=PASS pmm_recovered=PASS',
                    text)
                pass_m = re.search(
                    r'\[MEMORY COHORT\] PASS concurrent process cohorts and fragmented PMM headroom',
                    text)
                assert base_m and frag_m and rec_m and pass_m, f'Missing diagnostic marker in {uart}'
                assert len(cohort_matches) == 6, f'Expected 6 cohort passes (sizes 2, 4, 8 x 2 passes), found {len(cohort_matches)} in {uart}'
                case['baseline'] = {
                    'heap_used': int(base_m.group(1)),
                    'heap_committed': int(base_m.group(2)),
                    'heap_free': int(base_m.group(3)),
                    'largest_payload': int(base_m.group(4)),
                    'pmm_free': int(base_m.group(5)),
                    'tables': int(base_m.group(6)),
                    'deferred': int(base_m.group(7))
                }
                case['cohorts'] = []
                for cm in cohort_matches:
                    case['cohorts'].append({
                        'size': int(cm[0]),
                        'pass': int(cm[1]),
                        'peak_heap': int(cm[2]),
                        'post_heap': int(cm[3]),
                        'heap_committed': int(cm[4]),
                        'heap_delta': int(cm[5]),
                        'table_delta': int(cm[6]),
                        'result': cm[7]
                    })
                case['fragmented_pmm'] = {
                    'held_pages': int(frag_m.group(1)),
                    'scattered_pages': int(frag_m.group(2)),
                    'contig2_fail': 'PASS',
                    'contig4_fail': 'PASS',
                    'single_page': 'PASS',
                    'proc_spawn': 'PASS',
                    'oom_spawn_fail': 'PASS',
                    'contig_restore': 'PASS',
                    'post_spawn': 'PASS'
                }
                case['recovery'] = 'PASS'
            else:
                assert '[MEMORY COHORT]' not in text
            case['status'] = 'PASS'
            case['elapsed_seconds'] = round(time.monotonic() - start, 2)
            print(f'PASS memory cohort {label}: {uart}', flush=True)
        record['status'] = 'PASS'
    finally:
        record['after'] = {str(p): digest(p) for p in tracked}
        if record['after'] != before:
            record['status'] = 'FAIL'
        (out / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
        print(f"Memory cohort {record['status']}: {out}", flush=True)
        assert record['after'] == before


if __name__ == '__main__':
    main()
