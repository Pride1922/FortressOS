"""Reconcile a retained broad source guard against actual compiler dependencies.

Does not change the original manifest, logs or result. Unknown historical inputs
are listed separately; this does not invent a pre-run hash for generated C.
"""
import hashlib
import json
import pathlib
import subprocess
import sys

ROOT=pathlib.Path(__file__).resolve().parent.parent


def reconcile(folder):
    folder=folder.resolve()
    assert folder.is_relative_to((ROOT/'.codex-remote-attachments').resolve())
    manifest=json.loads((folder/'manifest.json').read_text())
    command=next(c for c in manifest['commands'] if c[0]=='gcc' and '-MM' not in c)
    dep=subprocess.run(command[:command.index('-o')]+['-MM','-MT','probe'],cwd=ROOT,
                       text=True,capture_output=True,timeout=60,check=True)
    (folder/'reconciled-dependencies.txt').write_text(dep.stdout+dep.stderr)
    paths={str((ROOT/p).resolve().relative_to(ROOT)) for p in dep.stdout.replace('\\\n',' ').split() if not p.endswith(':')}
    original=manifest.get('compiled_inputs',manifest.get('sources',manifest.get('inputs',{})))
    now={p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in sorted(paths)}
    changed=[p for p in now if p in original and now[p]!=original[p]]
    unknown=[p for p in now if p not in original]
    broad=manifest.get('changed_sources',manifest.get('changed_inputs',[]))
    result={'compiled_inputs_changed':changed,'compiled_inputs':now,
            'inputs_without_historical_hash':unknown,
            'original_broad_guard_changed_paths':broad,
            'broad_changes_outside_compiler_dependencies':[p for p in broad if p not in paths],
            'scope':'compiler-dependency source reconciliation only; original guard/output retained; not media or boot acceptance'}
    (folder/'preservation-reconciliation.json').write_text(json.dumps(result,indent=2)+'\n')
    assert not changed
    for name in paths:
        target=folder/'sources'/name
        target.parent.mkdir(parents=True,exist_ok=True)
        target.write_bytes((ROOT/name).read_bytes())
    print(json.dumps({k:v for k,v in result.items() if k!='compiled_inputs'},indent=2))


if __name__=='__main__':
    for argument in sys.argv[1:]:reconcile(pathlib.Path(argument))
