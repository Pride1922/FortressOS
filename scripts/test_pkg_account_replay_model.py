"""Run finite pure model checks; no kernel, media or filesystem mutation."""
from pathlib import Path
import hashlib
import importlib.util
import itertools
import json
import shutil
import time

ROOT = Path(__file__).resolve().parent.parent


def main():
    out = ROOT/'.codex-remote-attachments'/('pkg-account-model-'+time.strftime('%Y%m%d-%H%M%S'))
    out.mkdir(parents=True)
    paths = ['tests/pkg_account_replay_model.py', 'scripts/test_pkg_account_replay_model.py',
             'src/lib/account_redo.c', 'src/fs/ext4_write.inc', 'src/fs/ext4_orphan.inc',
             'src/fs/ext4_mutate.inc', 'src/include/account_redo.h']
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    before = {p: sha(ROOT/p) for p in paths}
    for p in paths:
        target = out/'sources'/p
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT/p, target)
        assert sha(target) == before[p]
    spec = importlib.util.spec_from_file_location('budget_model', out/'sources/tests/pkg_account_replay_model.py')
    model = importlib.util.module_from_spec(spec)
    import sys
    sys.modules[spec.name] = model
    spec.loader.exec_module(model)
    manifest = {'result': 'INCOMPLETE', 'scope': 'finite pure model, not actual EXT4 or formal universal proof',
                'sources': before, 'cases': []}
    try:
        manifest['mutation_checks'] = model.mutation_checks()
        for bs, layout, blocks, grow, order in itertools.product(
                (1024, 2048, 4096), ('compact', 'tight', 'mixed'), (1, 4, 5), (0, 1),
                itertools.permutations(range(3))):
            start = model.initial(bs, layout, blocks, order)
            result = model.explore(start, bs, blocks+grow)
            manifest['cases'].append(dict(block_size=bs, layout=layout, initial_blocks=blocks,
                                          growth_blocks=grow, order=order, **result))
            if len(manifest['cases']) % 36 == 0:
                print('Checked model profiles:', len(manifest['cases']), flush=True)
        assert any(c['admission'] == 'ACCEPT' for c in manifest['cases'])
        assert any(c['admission'] == 'REJECT' for c in manifest['cases'])
        assert any(c['admission'] == 'REJECT' and c['uninterrupted_completes'] for c in manifest['cases'])
        manifest['result'] = 'PASS'
    finally:
        manifest['changed_sources'] = [p for p, h in before.items() if sha(ROOT/p) != h]
        manifest['source_preservation'] = 'FAIL' if manifest['changed_sources'] else 'PASS'
        (out/'manifest.json').write_text(json.dumps(manifest, indent=2))
    assert manifest['source_preservation'] == 'PASS'
    print('PASS pure model:', len(manifest['cases']), 'profiles;',
          sum(c['states'] for c in manifest['cases']), 'states;',
          sum(c['admission'] == 'ACCEPT' for c in manifest['cases']), 'admissible profiles; evidence:', out)


if __name__ == '__main__':
    main()
