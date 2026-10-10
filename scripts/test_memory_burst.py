"""Representative process bursts and recovery under bounded PMM pressure; ISO-only QEMU diagnostics."""
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
    out = REPO / 'build' / ('memory-burst-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ'))
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
            label = f'{firmware}-{ram}-' + ('burst' if enabled else 'control')
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
                command += ['-fw_cfg', 'name=opt/fortress/memory_burst_test,string=1']
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
                    r'\[MEMORY BURST\] baseline heap_used=(\d+) heap_committed=(\d+) heap_free=(\d+) largest_payload=(\d+) pmm_free=(\d+) tables=(\d+) deferred=(\d+)',
                    text)
                post_m = re.search(
                    r'\[MEMORY BURST\] post_burst heap_used=(\d+) heap_committed=(\d+) heap_free=(\d+) largest_payload=(\d+) pmm_free=(\d+) tables=(\d+) deferred=(\d+) retained_heap_delta=(\d+) table_delta=(\d+)',
                    text)
                press_m = re.search(
                    r'\[MEMORY BURST\] pressure held_pages=(\d+) headroom_64=PASS proc_spawn_64=PASS headroom_2=PASS proc_spawn_oom=PASS heap_reuse=PASS',
                    text)
                rec_m = re.search(
                    r'\[MEMORY BURST\] recovery exact_heap=PASS exact_tables=PASS zero_deferred=PASS pmm_recovered=PASS',
                    text)
                pass_m = re.search(
                    r'\[MEMORY BURST\] PASS representative process bursts and recovery under bounded PMM pressure',
                    text)
                assert base_m and post_m and press_m and rec_m and pass_m, f'Missing diagnostic marker in {uart}'
                case['baseline'] = {
                    'heap_used': int(base_m.group(1)),
                    'heap_committed': int(base_m.group(2)),
                    'heap_free': int(base_m.group(3)),
                    'largest_payload': int(base_m.group(4)),
                    'pmm_free': int(base_m.group(5)),
                    'tables': int(base_m.group(6)),
                    'deferred': int(base_m.group(7))
                }
                case['post_burst'] = {
                    'heap_used': int(post_m.group(1)),
                    'heap_committed': int(post_m.group(2)),
                    'heap_free': int(post_m.group(3)),
                    'largest_payload': int(post_m.group(4)),
                    'pmm_free': int(post_m.group(5)),
                    'tables': int(post_m.group(6)),
                    'deferred': int(post_m.group(7)),
                    'retained_heap_delta': int(post_m.group(8)),
                    'table_delta': int(post_m.group(9))
                }
                case['pressure'] = {
                    'held_pages': int(press_m.group(1)),
                    'headroom_64_proc_spawn': 'PASS',
                    'headroom_2_proc_spawn_oom': 'PASS',
                    'heap_reuse': 'PASS'
                }
                case['recovery'] = 'PASS'
            else:
                assert '[MEMORY BURST]' not in text
            case['status'] = 'PASS'
            case['elapsed_seconds'] = round(time.monotonic() - start, 2)
            print(f'PASS memory burst {label}: {uart}', flush=True)
        record['status'] = 'PASS'
    finally:
        record['after'] = {str(p): digest(p) for p in tracked}
        if record['after'] != before:
            record['status'] = 'FAIL'
        (out / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
        print(f"Memory burst {record['status']}: {out}", flush=True)
        assert record['after'] == before


if __name__ == '__main__':
    main()
