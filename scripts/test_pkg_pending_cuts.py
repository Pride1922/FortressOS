"""Barrier cuts and selected sector tears across real recovery transactions."""
import argparse,datetime,json,os,pathlib,re,shutil,struct,subprocess
from concurrent.futures import ThreadPoolExecutor,as_completed
from pkg_enroll_records import H
from test_jbd2_replay_host import blocks
ROOT=pathlib.Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser();parser.add_argument('evidence');parser.add_argument('--jobs',type=int,choices=range(1,5),default=2);args=parser.parse_args();prior=pathlib.Path(args.evidence).resolve();old=json.loads((prior/'manifest.json').read_text())
    out=ROOT/'.codex-remote-attachments'/('pkg-pending-cuts-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'));out.mkdir();work=out/'workspace';sha=lambda p:H(p.read_bytes()).hex()
    guards={str(p.relative_to(ROOT)):sha(p) for folder in ('src','user','scripts','tests') for p in (ROOT/folder).rglob('*') if p.is_file() and '__pycache__' not in p.parts}
    guards.update({str(p.relative_to(ROOT)):sha(p) for p in [ROOT/'Makefile',ROOT/'AGENTS.md',ROOT/'PROTECTED.md',ROOT/'docs/plans/PKG2_PRODUCTION_INTEGRATION.md',*(ROOT/'bin').glob('*')] if p.is_file()})
    m={'runtime':'INCOMPLETE','preservation':'INCOMPLETE','boot_acceptance':'NOT RUN','commands':[],'cases':[],'guards':guards}
    def save():(out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n')
    def run(cmd,label,env=None):
        r=subprocess.run(cmd,cwd=work,capture_output=True,text=True,env=env,timeout=600);(out/(label+'.log')).write_text(r.stdout+r.stderr);assert not r.returncode,(label,r.stdout[-1200:],r.stderr[-2000:]);return r.stdout
    try:
        shutil.copytree(prior/'workspace',work)
        for name in ('tests/pkg_pending_fs.inc','tests/pkg_pending_candidate.c','tests/pkg_pending_candidate.h'):
            shutil.copyfile(ROOT/name,work/name)
        host=(ROOT/'tests/pkg_pending_fs_host.c').read_text()
        host=host.replace('ext4_engine_t *e=NULL;int r=native_open','d.trace=getenv("PKG_TRACE")!=NULL;\n    ext4_engine_t *e=NULL;int r=native_open')
        host=host.replace('d.cut=cut;assert(!native_open','d.cut=cut;d.after=getenv("PKG_AFTER")!=NULL;d.persistence=getenv("PKG_PERSIST")?atoi(getenv("PKG_PERSIST")):0;d.tear_bytes=getenv("PKG_TEAR")?atoi(getenv("PKG_TEAR")):0;d.trace=false;assert(!native_open')
        host=host.replace('puts("PASS native interrupted recovery and fresh-owner repeat");','FILE *result=fopen(argv[5],"wb");assert(result&&fwrite(stable,1,n,result)==n&&!fclose(result));\n        puts("PASS native interrupted recovery and fresh-owner repeat");')
        (work/'tests/pkg_pending_cuts_generated.c').write_text(host)
        m['compiled_inputs']={str(p.relative_to(work)):sha(p) for folder in ('src','user','scripts','tests') for p in (work/folder).rglob('*') if p.is_file() and '__pycache__' not in p.parts}
        cmd=next(c for c in old['commands'] if 'tests/pkg_pending_fs_host.c' in c);native=out/'cuts-native';cmd=[str(native) if x==str(prior/'pending-native') else 'tests/pkg_pending_cuts_generated.c' if x=='tests/pkg_pending_fs_host.c' else x for x in cmd];m['commands'].append(cmd);save();run(cmd,'compile')
        jobs=[];oracles={}
        for label in ('reserved','pending-staged','retiring'):
            original=prior/label;reference=out/(label+'-reference.img');basecmd=[str(native),str(original/'disk.img'),str(original/'capsule'),str(original/'selector'),str(original/'witness')]
            cmd=basecmd+[str(reference),'-1','1'];m['commands'].append(cmd);save();text=run(cmd,label+'-reference',dict(os.environ,PKG_TRACE='1'));raw=reference.read_bytes();root=out/(label+'-reference.root');root.write_bytes(raw[2048*512:(2048+65536)*512]);journal=blocks(root,'<8>')
            def masked(data,journal=journal):
                data=bytearray(data)
                for block in journal:data[(2048*512)+4096*block:(2048*512)+4096*(block+1)]=bytes(4096)
                return H(data).hex()
            oracles[label]=(masked(raw),journal)
            flush=[int(x) for x in re.findall(r'replay event=(\d+) op=flush',text)];assert len(flush)>=20
            for event in flush:
                for after in (False,True):jobs.append((label,basecmd,event,after,0,0))
            # Selected tears just before barriers, with all earlier writes
            # persistent. Recovery must authenticate the projected result.
            for event in flush[1::5]:jobs.append((label,basecmd,event-1,False,1,256))
        def case(job):
            label,basecmd,event,after,persist,tear=job;name=f'{label}-{event}-a{int(after)}-p{persist}-t{tear}';result=out/(name+'.img');env=dict(os.environ)
            if after:env['PKG_AFTER']='1'
            if persist:env['PKG_PERSIST']=str(persist)
            if tear:env['PKG_TEAR']=str(tear)
            cmd=basecmd+[str(result),str(event),'1'];run(cmd,name,env);raw=result.read_bytes();expected,journal=oracles[label];data=bytearray(raw)
            for block in journal:data[2048*512+4096*block:2048*512+4096*(block+1)]=bytes(4096)
            assert H(data).hex()==expected,(name,'non-journal disk mismatch')
            root=out/(name+'.root');root.write_bytes(raw[2048*512:(2048+65536)*512]);run(['e2fsck','-fn',str(root)],name+'-linux')
            value={'label':name,'command':cmd,'cut':event,'after':after,'persistence':persist,'tear_bytes':tear,'runtime':'PASS','disk_sha256':H(raw).hex(),'non_journal_disk_sha256':expected,'linux':'PASS'}
            result.unlink();root.unlink();return value
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            for future in as_completed([pool.submit(case,j) for j in jobs]):m['cases'].append(future.result());save()
        m['runtime']='PASS'
    except BaseException as e:m['runtime']='FAIL';m['error']=repr(e);raise
    finally:
        m['changed_inputs']=[p for p,h in guards.items() if sha(ROOT/p)!=h];m['changed_copies']=[p for p,h in m.get('compiled_inputs',{}).items() if sha(work/p)!=h]
        m['preservation']='FAIL' if m['changed_inputs'] or m['changed_copies'] else 'PASS';save();print(out,m['runtime'],m['preservation'],flush=True)
    assert m['preservation']=='PASS'
if __name__=='__main__':main()
