"""Private actual EXT4/JBD2 allocation+record coupling, regular-file fixtures."""
import datetime
import hashlib
import json
import pathlib
import re
import subprocess
from test_ext4_time_host import fixture

ROOT=pathlib.Path(__file__).resolve().parent.parent


def main():
    out=ROOT/'.codex-remote-attachments'/('pkg-grant-fs-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    out.mkdir(parents=True,exist_ok=False)
    sources=[p for folder in ('src','tests','user','scripts') for p in (ROOT/folder).rglob('*')
             if p.is_file() and p.suffix in ('.c','.h','.inc','.py','.mk') and '__pycache__' not in p.parts]
    sources += [ROOT/'AGENTS.md',ROOT/'README.md']
    sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    inventory={str(p.relative_to(ROOT)):sha(p) for p in sources}
    manifest={'result':'INCOMPLETE','sources':inventory,'cases':[],'commands':[],
        'scope':'private actual EXT4 inode/map/audit and JBD2 block charge/refund coupling; host sector-atomic callback cuts; no production caller, enrollment, boot or Ring3 acceptance'}
    def run(cmd,label,timeout=180):
        manifest['commands'].append(cmd)
        r=subprocess.run(cmd,cwd=ROOT,text=True,capture_output=True,timeout=timeout)
        with (out/(label+'.log')).open('a') as f:f.write(r.stdout+r.stderr)
        assert r.returncode==0,(label,r.returncode,r.stdout[-2000:],r.stderr[-2000:])
        return r.stdout+r.stderr
    try:
        cmd=['gcc','-std=c11','-O1','-g','-fsanitize=address,undefined','-fstack-usage',
             '-Wall','-Wextra','-Werror','-no-pie','-pthread','-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
        cmd+=['-I'+p for p in ('tests/ext4_host','tests/host','src/include','src/fs','src/drivers','src/mm','user/tools','.')]
        cmd+=['tests/pkg_grant_fs_host.c','tests/pkg_grant_candidate.c','tests/ext4_fault_disk.c',
              'src/fs/permission_values.c','src/kernel/creds.c','user/tools/digest.c','-o',str(out/'probe')]
        dep=run(cmd[:cmd.index('-o')]+['-MM','-MT','probe'],'dependencies')
        inputs={str((ROOT/p).resolve().relative_to(ROOT)) for p in dep.replace('\\\n',' ').split() if not p.endswith(':')}
        manifest['compiled_inputs']={p:sha(ROOT/p) for p in sorted(inputs)}
        run(cmd,'compile')
        for bs in (1024,2048,4096):
            source=fixture(out,bs); source_sha=sha(source)
            for ss in (512,4096):
                prefix=out/f'{bs}-{ss}'
                text=run([str(out/'probe'),str(source),str(ss),str(prefix)],f'{bs}-{ss}',timeout=900)
                cuts=int(re.search(r'cuts=(\d+)',text)[1])
                for suffix,q in (('charged',1),('refund',0),('recovered',None)):
                    image=pathlib.Path(str(prefix)+'-'+suffix+'.img')
                    run(['e2fsck','-fn',str(image)],f'{bs}-{ss}-linux')
                    dumped={}
                    for name in ('grant.v1','ownership.v1','payload'):
                        dst=out/f'{bs}-{ss}-{suffix}-{name}.dump'
                        run(['debugfs','-R',f'dump /pkg/{name} {dst}',str(image)],f'{bs}-{ss}-linux')
                        dumped[name]=dst.read_bytes()
                    import struct
                    ledger=dumped['grant.v1'];table=dumped['ownership.v1'];data=dumped['payload']
                    actual=struct.unpack_from('<Q',ledger,328)[0]
                    assert actual in (0,1) and (q is None or actual==q)
                    assert data==b'X'*(bs*actual)
                    assert struct.unpack_from('<Q',table,296)[0]==actual
                    assert hashlib.sha256(table).digest()==ledger[264:296]
                    assert hashlib.sha256(ledger[:992]).digest()==ledger[992:]
                    assert struct.unpack_from('<Q',ledger,312)[0]+actual==8
                assert sha(source)==source_sha
                manifest['cases'].append({'block':bs,'sector':ss,'cuts':cuts,'restarts_per_cut':2,'linux_audits':3,'result':'PASS'})
                print(manifest['cases'][-1],flush=True)
        manifest['result']='PASS'
    finally:
        manifest['changed_sources']=[name for name,h in inventory.items() if sha(ROOT/name)!=h]
        manifest['changed_compiled_inputs']=[p for p,h in manifest.get('compiled_inputs',{}).items() if sha(ROOT/p)!=h]
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    assert not manifest['changed_compiled_inputs']
    print('Evidence:',out)


if __name__=='__main__':main()
