#!/usr/bin/env python3
"""Actual DHCP service/CLI contention, private delayed-daemon ISO only.

BIOS/UEFI x SMP=1/4; one read-only ISO, user-mode e1000e, paired firmware.
No data disks or physical I/O. The delay/marker exists only in a copied source.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import time
from test_poll_nc import Guest

ROOT = Path(__file__).resolve().parents[1]

def boot(workspace, out, firmware, cpus):
    import test_poll_nc
    test_poll_nc.ROOT = workspace
    g = Guest(firmware, cpus, out)
    args = g.proc.args
    assert '-cdrom' in args and not any('/dev/' in arg for arg in args)
    assert not any('file=' in arg and not arg.endswith('.fd') for arg in args)
    label = f'{firmware}-smp{cpus}'
    (out / (label + '.argv.json')).write_text(json.dumps(args, indent=2) + '\n')
    def raw(cmd, timeout=30):
        start = len(g.data)
        g.send(cmd)
        return g.wait(lambda text: cmd + '\n' in text and text.endswith('$ '), start, timeout)
    def command(cmd, marker=None):
        text = raw(cmd)
        if marker:
            assert marker in text, (cmd, text)
        status = raw('echo $?')
        assert re.search(r'(?:^|\n)0\n', status), (cmd, text, status)
        return text
    def stop_daemon():
        command('sudo /bin/service stop dhcpc')
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if not re.search(r'(?:^|\n)\d+\n', raw('cat /run/dhcpc.pid', 5)):
                if 'State: stopped' in raw('sudo /bin/service status dhcpc', 5):
                    return
        raise AssertionError('DHCP service did not release its PID/lease')
    try:
        g.wait(lambda text: text.endswith('FortressOS login: '), timeout=90)
        g.send('operator')
        g.wait(lambda text: text.endswith('$ '), timeout=20)
        stop_daemon()
        raw('sudo /bin/rm /run/dhcp-acquire-held')
        command('sudo /bin/service start dhcpc')
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if '\nheld\n' in raw('cat /run/dhcp-acquire-held', 5):
                break
        else:
            raise AssertionError('Private daemon bind-hold marker missing')
        before = raw('cat /run/dhcpc.pid')
        owner = re.findall(r'(?:^|\n)(\d+)\n', before)[-1]
        command('sudo /sbin/dhcpc', 'via running DHCP daemon')
        after = raw('cat /run/dhcpc.pid')
        assert re.findall(r'(?:^|\n)(\d+)\n', after)[-1] == owner
        command('/sbin/ifconfig', '10.0.2.15/24')
        command('/sbin/dhcpc --status', 'running (PID')
        command('/bin/ping 10.0.2.2')
        # The free-port/no-service path must still acquire and start an owner.
        stop_daemon()
        text = command('sudo /sbin/dhcpc', 'acquired lease')
        assert 'via running DHCP daemon' not in text
        command('/sbin/dhcpc --status', 'running (PID')
        assert not any(word in g.text() for word in ('KERNEL PANIC', '[FATAL]', '[PROCESS FAULT]'))
        return {'firmware': firmware, 'cpus': cpus, 'result': 'PASS',
                'managed_contention': 'real service holds UDP 68; foreground joins without taking ownership',
                'standalone': 'free-port acquisition and daemon spawn PASS',
                'hardware': 'e1000e/user DHCP; no physical Wi-Fi acceptance'}
    finally:
        (out / (label + '.serial.log')).write_bytes(g.data)
        g.close()


def main():
    import tempfile
    parser = argparse.ArgumentParser()
    parser.add_argument('--workspace', type=Path)
    parser.add_argument('--cpus', nargs='+', type=int, default=[1, 4])
    parser.add_argument('--firmware', nargs='+', default=['bios', 'uefi'])
    args = parser.parse_args()
    assert set(args.cpus) <= {1, 4} and set(args.firmware) <= {'bios', 'uefi'}
    parent = ROOT / 'build/dhcp-contention'
    parent.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix='guest-', dir=parent))
    report = {'result': 'FAIL', 'cases': []}
    print(out, flush=True)
    try:
        if args.workspace:
            workspace = args.workspace.resolve()
        else:
            (ROOT / '.codex-remote-attachments/ext4-phase9').mkdir(parents=True, exist_ok=True)
            workspace = Path(subprocess.check_output(['python3', 'scripts/create_ext4_guest_workspace.py'], cwd=ROOT, text=True).strip())
        assert workspace.is_relative_to(ROOT / '.codex-remote-attachments')
        path = workspace / 'user/dhcpc.c'
        source = path.read_text()
        if not args.workspace:
            anchor = '    net_sockaddr_in_t bcast_addr;'
            assert anchor in source
            path.write_text(source.replace(anchor, '''    /* PRIVATE TEST ONLY: deterministic contention after the real bind. */
    static bool held;
    if (!held) {
        held = true;
        (void)write_file("/run/dhcp-acquire-held", "held\\n", 5);
        sleep_ms(3000);
    }

''' + anchor, 1))
        else:
            assert 'PRIVATE TEST ONLY: deterministic contention' in source
        report['workspace'] = str(workspace)
        with (out / 'build.log').open('w') as log:
            subprocess.run(['make', '-j4', 'bin/fortress.iso'], cwd=workspace,
                           stdout=log, stderr=subprocess.STDOUT, check=True, timeout=600)
        for firmware in args.firmware:
            for cpus in args.cpus:
                case = boot(workspace, out, firmware, cpus)
                report['cases'].append(case)
                print(case, flush=True)
        report['result'] = 'PASS'
    except Exception as error:
        report['failure'] = repr(error)
        raise
    finally:
        (out / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')

if __name__ == '__main__':
    main()
