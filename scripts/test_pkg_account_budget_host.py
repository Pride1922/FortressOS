"""Isolated actual account-redo protocol trace; no media/kernel build or grant implementation."""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent


def main():
    paths = ['tests/pkg_account_budget_host.c', 'src/lib/account_redo.c',
             'src/include/account_redo.h', 'src/include/types.h', 'src/include/string.h',
             'user/tools/digest.c', 'user/tools/digest.h',
             'scripts/test_pkg_account_budget_host.py']
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    before = {p: digest(ROOT/p) for p in paths}
    out = ROOT/'.codex-remote-attachments'/('pkg-account-budget-'+time.strftime('%Y%m%d-%H%M%S'))
    work = out/'workspace'
    work.mkdir(parents=True)
    for p in paths:
        dest = work/p
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT/p, dest)
        assert digest(dest) == before[p]
    manifest = {'sources': before, 'scope': 'actual redo, modelled data-space adapter',
                'ext4_acceptance': 'NOT RUN', 'grant_acceptance': 'NOT RUN',
                'boot_acceptance': 'NOT RUN', 'result': 'INCOMPLETE'}
    try:
        with (out/'test.log').open('w') as log:
            subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                            '-Isrc/include', 'tests/pkg_account_budget_host.c',
                            'src/lib/account_redo.c', 'user/tools/digest.c', '-o', 'probe'],
                           cwd=work, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
            subprocess.run([str(work/'probe')], cwd=work, stdout=log, stderr=subprocess.STDOUT,
                           check=True, timeout=60)
        manifest['result'] = 'PASS'
    finally:
        manifest['changed_sources'] = [p for p in paths if digest(ROOT/p) != before[p]]
        manifest['source_preservation'] = 'FAIL' if manifest['changed_sources'] else 'PASS'
        (out/'manifest.json').write_text(json.dumps(manifest, indent=2))
    assert manifest['source_preservation'] == 'PASS'
    print((out/'test.log').read_text(), end='')
    print('Evidence:', out)


if __name__ == '__main__':
    main()
