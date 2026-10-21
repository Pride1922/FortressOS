"""Private finite /etc namespace admission model; no production behavior change."""
from pathlib import Path
import hashlib
import importlib
import json
import shutil
import sys
import time

ROOT = Path(__file__).resolve().parent.parent


def main():
    out = ROOT/'.codex-remote-attachments'/('pkg-namespace-admission-'+time.strftime('%Y%m%d-%H%M%S'))
    work = out/'sources'
    work.mkdir(parents=True)
    paths = ['tests/pkg_namespace_admission.py', 'tests/pkg_account_replay_model.py',
             'scripts/test_pkg_namespace_admission.py', 'src/fs/ext4_write.inc',
             'src/lib/account_redo.c', 'src/fs/ext4_accounts.inc']
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    before = {p: sha(ROOT/p) for p in paths}
    for name in paths:
        target = work/name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT/name, target)
        assert sha(target) == before[name]
    sys.path.insert(0, str(work/'tests'))
    result = {'result': 'INCOMPLETE'}
    try:
        result = importlib.import_module('pkg_namespace_admission').run()
    except Exception as error:
        result.update(result='FAIL', error=repr(error))
        raise
    finally:
        result['sources'] = before
        result['source_preservation'] = all(sha(ROOT/p) == h for p, h in before.items())
        (out/'manifest.json').write_text(json.dumps(result, indent=2)+'\n')
    print('PASS', len(result['cases']), 'profiles;', sum(r['states'] for r in result['cases']),
          'state instances; evidence:', out, flush=True)


if __name__ == '__main__':
    main()
