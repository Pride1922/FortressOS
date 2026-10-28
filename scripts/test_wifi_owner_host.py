#!/usr/bin/env python3
"""Actual copied Wi-Fi owner/SYS_WIFI/WLAN logic; mocked radio and scheduler."""
import json
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [
    'tests/wifi_owner_host.c', 'src/kernel/handoff.c', 'src/net/net_control.c',
    'src/net/net_wifi_owner.c', 'src/net/wlan/wlan_mgr.c',
    'src/net/wlan/net_80211.c', 'src/net/wlan/wlan_crypto.c', 'src/net/wlan/wlan_wpa2.c',
    'src/net/net.c', 'src/net/net_ping.c', 'src/net/net_ipv4.c', 'src/net/udp.c',
    'src/net/icmp.c', 'src/net/checksum.c', 'src/net/ipv4.c', 'src/net/eth.c', 'src/net/arp.c',
]

def main():
    parent = ROOT / 'build/wifi-owner'
    parent.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix='host-', dir=parent))
    print(out, flush=True)
    manifest = {'result': 'FAIL', 'runs': [], 'sources': {}}
    try:
        for sanitizer in ('address,undefined', 'thread'):
            exe = out / sanitizer.replace(',', '-')
            build = ['gcc', '-std=c11', '-O1', '-g', '-DTEST_SMP_MEMORY', '-DWIFI_OWNER_HOST',
                     '-pthread', '-fsanitize=' + sanitizer, '-no-pie', '-Wall', '-Wextra', '-Werror',
                     '-Isrc/include', '-iquote', 'src/kernel', '-Isrc/drivers', '-Isrc/arch/x86_64',
                     '-Isrc/mm', '-Isrc/net', *SOURCES, '-o', str(exe)]
            argv = ['setarch', 'x86_64', '-R', str(exe)] if sanitizer == 'thread' else [str(exe)]
            record = {'build': build, 'run': argv}
            manifest['runs'].append(record)
            dependencies=subprocess.check_output(build[:-2]+['-MM'],cwd=ROOT,text=True)
            (out/(exe.name+'.dependencies')).write_text(dependencies)
            for line in dependencies.replace('\\\n',' ').splitlines():
                for name in line.split(':',1)[1].split():
                    path=ROOT/name
                    if path.is_file(): manifest['sources'][name]=hashlib.sha256(path.read_bytes()).hexdigest()
            with (out / (exe.name + '.build.log')).open('w') as log:
                subprocess.run(build, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
            with (out / (exe.name + '.log')).open('w') as log:
                subprocess.run(argv, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True,
                               timeout=120, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1:halt_on_error=1'})
            record['result'] = 'PASS'
            print((out / (exe.name + '.log')).read_text(), flush=True)
        assert all(hashlib.sha256((ROOT/name).read_bytes()).hexdigest()==sha
                   for name,sha in manifest['sources'].items())
        manifest['result'] = 'PASS'
    finally:
        (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')

if __name__ == '__main__':
    main()
