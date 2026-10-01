#!/usr/bin/env python3
"""Compare idle tick accounting with an isolated, instrumented Phase 4 kernel.

Only adds the same read-only worker tick observation to the archived baseline.
No source or build artifacts in the current checkout are replaced; no data disks.
"""
import re
import argparse
import shutil
import subprocess
import tarfile
import tempfile
import time
from pathlib import Path
import test_net_eth
from test_net_pci import REPO, CODE, VARS

BASELINE = '3381d34'
OBSERVATION = '''        if (!s_idle_reported) {
            uint64_t now=apic_timer_get_bsp_ticks();
            if (count || s_test_pending || s_probe_pending || !s_idle_start) {
                s_idle_start=now; s_idle_ticks=thread_current()->total_ticks;
            } else if (now-s_idle_start>=5*apic_timer_get_frequency()) {
                serial_puts("[NET 5] Idle worker CPU ticks/elapsed ticks/hz (hex): ");
                serial_print_hex(thread_current()->total_ticks-s_idle_ticks); serial_puts("/");
                serial_print_hex(now-s_idle_start); serial_puts("/");
                serial_print_hex(apic_timer_get_frequency()); serial_puts("\\n");
                s_idle_reported=true;
            }
        }
'''
PATTERN = r'Idle worker CPU ticks/elapsed ticks/hz \(hex\): (0x[0-9A-Fa-f]+)/(0x[0-9A-Fa-f]+)/(0x[0-9A-Fa-f]+)'


def compare():
    for mode in ('bios', 'uefi'):
        paths = [REPO / 'build' / f'test-net-idle-phase4-{mode}.log',
                 REPO / 'build' / f'test-net-udp-{mode}-e1000-user-smp1.log']
        matches = [re.search(PATTERN, p.read_text(errors='replace')) for p in paths]
        assert all(matches), f'missing idle observation: {paths}'
        old, new = [[int(x, 16) for x in m.groups()] for m in matches]
        assert old[2] == new[2] == 100
        assert old[1] >= 500 and new[1] >= 500
        assert old[0] < old[1] // 10 and new[0] < new[1] // 10
        print(f'[PASS] {mode} e1000 SMP=1 idle: instrumented Phase 4 {old[0]}/{old[1]} ticks; '
              f'Phase 5 {new[0]}/{new[1]} ticks; 100 Hz (quantized accounting, not zero CPU cost)', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compare-only', action='store_true', help='compare saved observations without rebuilding baseline')
    args = parser.parse_args()
    if args.compare_only:
        compare(); return
    for mode in ('bios', 'uefi'):
        path = REPO / 'build' / f'test-net-udp-{mode}-e1000-user-smp1.log'
        assert path.exists() and re.search(PATTERN, path.read_text(errors='replace')), 'complete Phase 5 matrix first'
    with tempfile.TemporaryDirectory(prefix='fortress-phase4-idle-') as name:
        tmp = Path(name); checkout = tmp / 'baseline'; checkout.mkdir()
        archive = tmp / 'source.tar'
        with archive.open('wb') as out:
            subprocess.run(['git', 'archive', BASELINE], cwd=REPO, stdout=out, check=True, timeout=30)
        with tarfile.open(archive) as tar:
            tar.extractall(checkout, filter='data')
        shutil.copytree(REPO / 'limine', checkout / 'limine')
        source = checkout / 'src/net/net.c'; text = source.read_text()
        marker = '        if (count==64) thread_yield();'
        assert text.count(marker) == 1
        text = text.replace('static bool s_probe_pending;',
                            'static bool s_probe_pending;\nstatic uint64_t s_idle_start, s_idle_ticks;\nstatic bool s_idle_reported;')
        source.write_text(text.replace(marker, OBSERVATION + marker))
        build_log = REPO / 'build/test-net-idle-baseline-build.log'
        with build_log.open('wb') as out:
            subprocess.run(['make', '-j4', 'bin/fortress.iso'], cwd=checkout, stdout=out, stderr=out, check=True, timeout=240)
        test_net_eth.REPO = checkout
        iso = test_net_eth.build_iso(tmp, 'net_test=arp')
        for mode in ('bios', 'uefi'):
            log = REPO / 'build' / f'test-net-idle-phase4-{mode}.log'
            variables = tmp / f'{mode}.fd'
            if mode == 'uefi': shutil.copyfile(VARS, variables)
            cmd = ['qemu-system-x86_64', '-M', 'q35', '-m', '2G', '-accel', 'tcg', '-smp', '1',
                   '-display', 'none', '-monitor', 'none', '-no-reboot', '-boot', 'd', '-cdrom', str(iso),
                   '-serial', f'file:{log}', '-netdev', 'user,id=net0', '-device', 'e1000,netdev=net0']
            if mode == 'uefi':
                cmd += ['-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
                        '-drive', f'if=pflash,format=raw,unit=1,file={variables}']
            allowed = set(cmd)
            assert '-blockdev' not in allowed and not any('nvme' in x or 'usb-storage' in x for x in cmd)
            for i, value in enumerate(cmd):
                if value == '-drive':
                    assert cmd[i + 1] in (f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
                                          f'if=pflash,format=raw,unit=1,file={variables}')
            log.write_text(''); proc = None
            with log.with_suffix('.stderr').open('wb') as stderr:
                try:
                    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=stderr)
                    deadline = time.monotonic() + 60
                    while time.monotonic() < deadline:
                        value = log.read_text(errors='replace'); match = re.search(PATTERN, value)
                        if match and 'fortress>' in value: break
                        assert proc.poll() is None
                        time.sleep(.1)
                    else: raise AssertionError(value[-5000:])
                    ticks, elapsed, hz = [int(x, 16) for x in match.groups()]
                    assert hz == 100 and elapsed >= 5 * hz and ticks < elapsed // 10
                    print(f'[BASELINE] {mode}: {ticks}/{elapsed} ticks at {hz} Hz', flush=True)
                finally:
                    if proc is not None:
                        proc.terminate()
                        try: proc.wait(timeout=3)
                        except subprocess.TimeoutExpired: proc.kill(); proc.wait()
    compare()


if __name__ == '__main__':
    main()
