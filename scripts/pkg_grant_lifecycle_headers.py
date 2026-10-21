"""Retain header-only allocated-section reconciliation, never rewrite a guard."""
import hashlib
import json
import pathlib
import shutil
import struct
import subprocess
import sys

ROOT=pathlib.Path(__file__).resolve().parent.parent


def allocated(path):
    data=path.read_bytes();assert data[:6]==b'\x7fELF\x02\x01'
    offset=struct.unpack_from('<Q',data,40)[0]
    stride,count,strings=struct.unpack_from('<HHH',data,58)
    headers=[struct.unpack_from('<IIQQQQIIQQ',data,offset+i*stride) for i in range(count)]
    sh=headers[strings];names=data[sh[4]:sh[4]+sh[5]];result={}
    for h in headers:
        name=names[h[0]:].split(b'\0',1)[0].decode()
        if not h[2]&2 or name=='.note.gnu.build-id':continue
        result[name]={'type':h[1],'flags':h[2],'address':h[3],'size':h[5],
                      'sha256':None if h[1]==8 else hashlib.sha256(data[h[4]:h[4]+h[5]]).hexdigest()}
    return result


def reconcile(folder):
    folder=folder.resolve();assert folder.is_relative_to((ROOT/'.codex-remote-attachments').resolve())
    manifest=json.loads((folder/'manifest.json').read_text())
    assert not manifest['changed_compiled_copies']
    names=manifest['changed_compiled_repository_sources']
    assert names and set(names)<= {'src/drivers/console.h','src/drivers/input.h'}
    out=folder/'header-reconciliation';work=out/'workspace'
    shutil.copytree(folder/'workspace',work)
    command=next(c for c in manifest['commands'] if c[0]=='gcc' and '-MM' not in c)
    base=command[:command.index('-o')]
    def build(name):
        path=out/name
        r=subprocess.run(base+['-o',str(path)],cwd=work,text=True,capture_output=True,timeout=180)
        (out/(name+'.log')).write_text(r.stdout+r.stderr);assert r.returncode==0
        return allocated(path)
    before=build('before')
    changed={}
    for name in names:
        old=(work/name).read_bytes();new=(ROOT/name).read_bytes()
        assert hashlib.sha256(old).hexdigest()==manifest['sources'][name]
        # Narrow this evidence to additive declarations; no conditional/types
        # or existing declaration changes can silently qualify as equivalent.
        additions=[line for line in new.decode().splitlines() if line not in old.decode().splitlines()]
        assert additions and all(line.startswith(('bool console_is_mouse_reporting(', 'void input_inject_bytes(')) for line in additions)
        assert all(line in new.decode().splitlines() for line in old.decode().splitlines())
        (work/name).write_bytes(new)
        changed[name]={'before':hashlib.sha256(old).hexdigest(),'after':hashlib.sha256(new).hexdigest(),'added_lines':additions}
    after=build('after');assert before==after
    (out/'manifest.json').write_text(json.dumps({'result':'PASS','headers':changed,
        'allocated_sections':before,'scope':'same copied program rebuilt before/after additive unused declarations; all SHF_ALLOC sections match except build-id; original source guard failure retained; not full-current-tree or boot acceptance'},indent=2)+'\n')
    print('PASS allocated-section header reconciliation:',folder.name)


if __name__=='__main__':
    for name in sys.argv[1:]:reconcile(pathlib.Path(name))
