"""Selected full depth-2 cleanup cuts using an accepted disposable fixture."""
import argparse,hashlib,json,tempfile
from pathlib import Path
from create_ext4_fixtures import ROOT,run
from test_jbd2_replay_host import oracle

def main():
    parent=ROOT/'build/ext4-phase8-4'
    parser=argparse.ArgumentParser();parser.add_argument('--fixtures',type=Path);args=parser.parse_args();origin=None
    if args.fixtures:
        fixture=args.fixtures.resolve();assert fixture.is_dir() and fixture.is_relative_to(parent.resolve())
        records=[];origin=str(fixture)
        for bs in (1024,2048,4096):
            for wrap in (False,True):
                source=fixture/f'{bs}-{"wrap" if wrap else "normal"}.img';config=fixture/f'config-{bs}.bin'
                assert source.is_file() and source.stat().st_size==32*1024*1024
                assert config.is_file() and config.stat().st_size==28
                for ss in (512,4096):records.append({'block':bs,'sector':ss,'wrap':wrap,
                    'argv':['fixture',str(source),str(config),str(ss)],'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest()})
    else:
        candidates=sorted(parent.glob('run-*/manifest.json'),key=lambda p:p.stat().st_mtime,reverse=True)
        assert candidates,'Run test-ext4-orphan-host first, or supply an explicit disposable --fixtures directory.'
        origin=str(candidates[0]);records=json.loads(candidates[0].read_text())
    assert len(records)==12
    record=next(r for r in records if r['block']==1024 and r['sector']==512 and not r['wrap'])
    source,config=map(Path,record['argv'][1:3])
    for path in (source,config):assert path.is_file() and path.resolve().is_relative_to(parent.resolve())
    assert hashlib.sha256(source.read_bytes()).hexdigest()==record['source_sha256']
    out=Path(tempfile.mkdtemp(prefix='deep-run-',dir=parent));prefix=out/'deep'
    admissions=[]
    for item in records:
        cmd=[str(ROOT/'build/ext4_orphan_deep_host'),*item['argv'][1:4],str(prefix),'--admission-check']
        text=run(cmd,timeout=60);(out/f'admit-{item["block"]}-{item["sector"]}-{item["wrap"]}.log').write_text(text)
        print(text,end='',flush=True);admissions.append(cmd)
    cmd=[str(ROOT/'build/ext4_orphan_deep_host'),str(source),str(config),'512',str(prefix)]
    text=run(cmd,timeout=600);(out/'host.log').write_text(text);print(text,end='',flush=True)
    for phase in ('pending','complete','recovered'):
        image=Path(f'{prefix}-{phase}.img');linux=Path(str(image)+'.linux.img')
        oracle(image,linux,Path(str(image)+'.linux.log'))
        assert 'File not found' in run(['debugfs','-R','stat /deep.bin',str(linux)])
    (out/'manifest.json').write_text(json.dumps({'argv':cmd,'admission_argv':admissions,'fixture_origin':origin,
        'source_sha256':record['source_sha256']},indent=2)+'\n')
    print(f'EXT4 full depth-2 host PASS, 3 Linux oracle copies; evidence: {out}')

if __name__=='__main__':main()
