"""EH-6 actual mounted directory moves, disposable files, Linux oracle."""
from pathlib import Path
import argparse,hashlib,json,re,subprocess,tempfile
from create_ext4_fixtures import ROOT,run
from test_ext4_time_host import fixture
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--out',type=Path,default=ROOT/'build/ext4-mv-host');args=parser.parse_args()
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    paths=[p for p in (ROOT/'src/fs').iterdir() if p.is_file()]+[ROOT/p for p in ('tests/ext4_mv_host.c','scripts/test_ext4_mv_host.py','tests/ext4_mount_host.c','tests/ext4_fault_disk.c')]
    sources={str(p.relative_to(ROOT)):sha(p) for p in paths}
    cmd=['gcc','-std=c11','-O1','-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-no-pie','-pthread','-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
    cmd+=['-I'+p for p in ('tests/ext4_host','tests/host','src/include','src/fs','src/drivers','src/mm')]
    cmd+=['tests/ext4_mv_host.c','tests/ext4_time_clock.c','tests/ext4_fault_disk.c','src/fs/permission_values.c','src/kernel/creds.c','-o',str(out/'probe')];run(cmd)
    results=[];legacy_results=[]
    with tempfile.TemporaryDirectory(prefix='ext4-mv-') as tmp:
        folder=Path(tmp)
        for bs in (1024,2048,4096):
            base=fixture(folder,bs);digest=sha(base)
            for ss in (512,4096):
                prefix=folder/f'{bs}-{ss}'
                p=subprocess.run([str(out/'probe'),str(base),str(ss),str(prefix)],capture_output=True,text=True,timeout=1200)
                (out/f'{bs}-{ss}.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stdout+p.stderr
                audit=''
                for suffix,path,parent in (('moved','/var/moved','/var'),('back','/opt/back','/opt'),('recovered','/var/moved','/var')):
                    image=Path(str(prefix)+f'-{suffix}.img');audit+=run(['e2fsck','-fn',str(image)])
                    text=run(['debugfs','-R',f'stat {parent}',str(image)]);audit+=text;ino=int(re.search(r'Inode:\s+(\d+)',text)[1])
                    listing=run(['debugfs','-R',f'ls -l {path}',str(image)]);audit+=listing
                    assert re.search(rf'^\s*{ino}\s+.*\s\.\.\s*$',listing,re.M),listing
                    payload=folder/'payload';audit+=run(['debugfs','-R',f'dump {path}/sub/data {payload}',str(image)])
                    assert payload.read_bytes()==b'PAYLOAD';payload.unlink()
                (out/f'{bs}-{ss}-linux.log').write_text(audit);assert sha(base)==digest
                results.append({'block':bs,'sector':ss,'result':'PASS','cuts':int(re.search(r'PASS (\d+) rename',p.stdout)[1]),'linux_audits':3});print(results[-1],flush=True)
            legacy=folder/f'legacy-{bs}.img';legacy.write_bytes(base.read_bytes());run(['tune2fs','-f','-O','^has_journal',str(legacy)])
            prefix=folder/f'legacy-{bs}-result';p=subprocess.run([str(out/'probe'),str(legacy),'512',str(prefix),'legacy'],capture_output=True,text=True,timeout=120)
            (out/f'legacy-{bs}.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stdout+p.stderr
            (out/f'legacy-{bs}-linux.log').write_text(run(['e2fsck','-fn',str(prefix)+'-legacy.img']))
            legacy_results.append({'block':bs,'sector':512,'result':'PASS','linux_audits':1})
    assert all(sha(ROOT/p)==h for p,h in sources.items())
    (out/'manifest.json').write_text(json.dumps({'results':results,'legacy_results':legacy_results,'sources':sources,'binary_sha256':sha(out/'probe'),'command':cmd},indent=2)+'\n')
if __name__=='__main__':main()
