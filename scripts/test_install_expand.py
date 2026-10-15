"""Actual installer expansion plus Linux audits on sparse regular files only."""
import argparse,json,shutil,tempfile
from pathlib import Path
import test_root_phase0 as fixture
ROOT=Path(__file__).resolve().parents[1]
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--templates',type=Path,required=True);args=parser.parse_args()
    out=Path(tempfile.mkdtemp(prefix='expand-',dir=ROOT/'build/root-phase4'))
    manifest={'result':'FAIL','scope':'actual expansion sanitizer and Linux fsck; no physical or guest claim','cases':[]}
    def run(cmd):return fixture.run(list(map(str,cmd)),out/'host.log',600)
    try:
        binary=out/'host';run(['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-no-pie',
            '-Itests/host','-Isrc/include','tests/install_expand_host.c','src/lib/install_expand.c','-o',binary])
        seed=args.templates/'root.img';before=fixture.digest(seed)
        with tempfile.TemporaryDirectory(prefix='fortress-expand-') as tmp:
            for size in (318*1024**2-33*512,446*1024**2-33*512,8*1024**3,256*1024**3,1024*1024**3):
                root=Path(tmp)/'root.img';shutil.copyfile(seed,root)
                run([binary,root,size]);run(['e2fsck','-fn',root]);header=run(['dumpe2fs','-h',root])
                manifest['cases'].append({'bytes':size,'result':'PASS','header':header});root.unlink()
        assert fixture.digest(seed)==before
        manifest['result']='PASS';print(out,flush=True)
    finally:(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
if __name__=='__main__':main()
