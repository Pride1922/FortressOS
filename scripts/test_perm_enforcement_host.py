"""Actual filesystem actor admission and metadata/content atomic-cut inventory.
All fixtures and Linux audits are disposable regular files; no physical I/O.
"""
from pathlib import Path
import hashlib,json,subprocess,tempfile,sys
ROOT=Path(__file__).resolve().parent.parent
source=Path(sys.argv[1]).resolve()
assert source.is_relative_to(ROOT/'build/permissions-phase0') and source.is_dir()
out=Path(tempfile.mkdtemp(prefix='enforcement-',dir=ROOT/'build/permissions-phase2'))
common=['gcc','-std=c11','-O1','-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-no-pie','-pthread','-Itests/host','-Isrc/include','-Isrc/fs','-Isrc/drivers','-Isrc/mm']
sources={p.relative_to(ROOT).as_posix():hashlib.sha256(p.read_bytes()).hexdigest()
         for base in ('src','tests') for p in (ROOT/base).rglob('*')
         if p.suffix in ('.c','.h','.inc')}
def run(args,timeout=900):
    r=subprocess.run(args,cwd=ROOT,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=timeout)
    with (out/'results.log').open('a') as log:log.write(json.dumps(args)+'\n'+r.stdout)
    print(r.stdout,end='',flush=True);r.check_returncode();return r.stdout
for fs in ('ext2','ext4'):
    args=common+(['-Itests/ext4_host'] if fs=='ext4' else [])+[f'tests/perm_{fs}_enforcement_host.c']
    if fs=='ext4':args+=['tests/ext4_fault_disk.c']
    args+=['src/fs/permission_values.c','src/kernel/creds.c','-o',str(out/fs)]
    run(args)
    for size in ((128,256) if fs=='ext2' else (256,)):
        dest=out/f'{fs}-{size}'
        run([str(out/fs),str(source/f'{fs}-{size}.img'),str(dest)])
        result=Path(str(dest)+'-acceptance.img') if fs=='ext4' else dest
        run(['e2fsck','-fn',str(result)])
        stat=run(['debugfs','-R','stat /public/mode',str(result)])
        assert 'Mode:  0770' in stat or 'Mode:  0770' in stat.replace('Mode: ','Mode:  '),stat
        assert '4294967295' in stat or 'User:    -1' in stat,stat
run([str(out/'ext4'),str(source/'ext4-256.img'),str(out/'cuts'),'--cuts'])
for kind in range(4):
    for state in ('old','new'):
        p=out/f'cuts-op{kind}-{state}.img';run(['e2fsck','-fn',str(p)]);run(['debugfs','-R','stat /metadata',str(p)])
assert all(hashlib.sha256((ROOT/p).read_bytes()).hexdigest()==value for p,value in sources.items()),'sources changed during run; preserve results, rerun final inventory'
(out/'manifest.json').write_text(json.dumps({'source':str(source),'files':sources,'scope':'filesystem pthread adapters and atomic-sector cuts; no IRQ/guest/physical proof'},indent=2)+'\n')
print(f'PASS permissions enforcement filesystem inventory; evidence {out}',flush=True)
