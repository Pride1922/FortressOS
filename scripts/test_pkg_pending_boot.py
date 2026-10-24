"""Private Ring 3 recovery, native oracle, BIOS/UEFI x SMP=1/4."""
import argparse,inspect,json,pathlib,re,struct,subprocess,sys,tempfile
from concurrent.futures import ThreadPoolExecutor,as_completed
import test_pkg_boot_admission as runner
from pkg_pending_boot_overlay import overlay
from pkg_pending_records import prepare
from pkg_enroll_fixture import create,gpt
from pkg_enroll_records import write_fresh_records,H

def build_pending(image,out,roles,kernel,bootstrap):
    return prepare(image,pathlib.Path(out),roles,kernel,bootstrap)

# Retain the established serial/login/firmware runner, adding explicit native
# expected counts for mutation. Its old replay-only assertions remain intact.
source=inspect.getsource(runner.boot).replace('def boot(','def recovery_boot(')
source=source.replace('sequence=1):','sequence=1,expected_counts=None):')
source=source.replace('((16,2) if expected else (0,0))','(expected_counts if expected else (0,0))')
source=source.replace("f'PKGBOOT ADMITTED IDLE={sequence} detached'","'PKGBOOT ADMITTED RECOVERY detached'")
source=source.replace("            (dest/'result.json').write_text",
'''            statuses=[tuple(map(int,x)) for x in re.findall(r'PKGBOOT STATUS phase=(\\d+) sequence=(\\d+)',text())]
            if not refused:assert len(statuses)==2 and statuses[-1][0]==0
            result.update(initial_phase=statuses[0][0] if statuses else None,initial_sequence=statuses[0][1] if statuses else None,sequence=statuses[-1][1] if statuses else None)
            (dest/'result.json').write_text''')
scope=dict(vars(runner));exec(compile(source,'<private pending boot runner>','exec'),scope);recovery_boot=scope['recovery_boot']

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--native',required=True);parser.add_argument('--smoke',action='store_true');parser.add_argument('--jobs',type=int,choices=range(1,5),default=4);parser.add_argument('--reuse-build',help='reuse an immutable completed private build; preserve its original guard result');args=parser.parse_args()
    native=pathlib.Path(args.native).resolve();assert native.is_file()
    if args.reuse_build:out=pathlib.Path(args.reuse_build).resolve()
    else:
        runner.overlay=overlay;runner.build=build_pending
        before=set((runner.ROOT/'.codex-remote-attachments').glob('pkg-boot-admission-*'))
        saved=sys.argv;sys.argv=[sys.argv[0],'--build-only']
        try:runner.main()
        finally:sys.argv=saved
        added=set((runner.ROOT/'.codex-remote-attachments').glob('pkg-boot-admission-*'))-before;assert len(added)==1
        out=added.pop()
    work=out/'workspace';iso=out/'valid.iso';base=json.loads((out/'manifest.json').read_text())
    assert not base.get('error') and base['compiled_inputs'] and not base['changed_copies']
    assert all(runner.H(work/p)==h for p,h in base['compiled_inputs'].items())
    for name,path in (('kernel',work/'bin/fortress.elf'),('bootstrap',out/'bootstrap.tar'),('selector',out/'selector.bin'),('policy',out/'policy.bin')):assert runner.H(path)==base['measurements'][name]
    assert not (out/'recovery-manifest.json').exists(),'do not overwrite a previous runtime campaign'
    m={'boot_runtime':'INCOMPLETE','preservation':'INCOMPLETE','native_oracle':str(native),'cases':[],'commands':[],'base_build_manifest':str(out/'manifest.json'),'build_preservation':base['preservation']}
    def save():(out/'recovery-manifest.json').write_text(json.dumps(m,indent=2)+'\n')
    jobs=[]
    try:
        with tempfile.TemporaryDirectory(prefix='fortress-pending-boot-') as t:
            t=pathlib.Path(t);roles={}
            for name,values in (('selector',(111,112,113)),('enrollment',(41,42,43)),('catalog',(1,2,3))):
                roles[name]=[]
                for v in values:p=t/f'{name}-{v}';p.write_bytes(bytes([v])*32);p.chmod(0o600);roles[name].append(p)
            for label,phase,published,installed in (('reserved',1,False,False),('pending-staged',2,False,False),('pending-published',2,True,False),('pending-state',2,True,True),('retiring',3,True,True)):
                fixture=t/label;fixture.mkdir();image,_=create(fixture,identities=True)
                p=prepare(image,fixture/'prepared',roles,bytes.fromhex(base['measurements']['kernel']),bytes.fromhex(base['measurements']['bootstrap']),phase,published,installed)
                assert p['selector']==(out/'selector.bin').read_bytes() and p['policy']==(out/'policy.bin').read_bytes()
                write_fresh_records(image,p);disk=out/(label+'.img');disk.write_bytes(gpt(image.read_bytes()))
                witness=out/(label+'.witness');witness.write_bytes(p['witness']);expected=out/(label+'-expected.img')
                cmd=[str(native),str(disk),str(out/'policy.bin'),str(out/'selector.bin'),str(witness),str(expected),'-1','1'];m['commands'].append(cmd);save()
                r=subprocess.run(cmd,cwd=work,capture_output=True,text=True,timeout=240);(out/(label+'-oracle.log')).write_text(r.stdout+r.stderr);assert not r.returncode,(r.stdout,r.stderr)
                counters=re.search(r'PASS native recovery writes=(\d+) flushes=(\d+)',r.stdout);assert counters;counts=tuple(map(int,counters.groups()))
                for fw,smp in ([('bios',1)] if args.smoke else [('bios',1),('bios',4),('uefi',1),('uefi',4)]):
                    jobs.append((label,phase,fw,smp,disk,expected,counts))
            def pair(job):
                label,phase,fw,smp,disk,expected,counts=job;name=f'{label}-{fw}-{smp}'
                first=recovery_boot(out,work,iso,disk,fw,smp,name,expected=expected,expected_counts=counts)
                second=recovery_boot(out,work,iso,out/name/'enrolled.img',fw,smp,name+'-reboot')
                for value,folder,initial in ((first,name,phase),(second,name+'-reboot',0)):
                    statuses=[tuple(map(int,x)) for x in re.findall(r'PKGBOOT STATUS phase=(\d+) sequence=(\d+)',(out/folder/'serial.log').read_text())]
                    assert len(statuses)==2 and statuses[0][0]==initial and statuses[1][0]==0
                    value.update(initial_phase=statuses[0][0],initial_sequence=statuses[0][1],sequence=statuses[1][1]);(out/folder/'result.json').write_text(json.dumps(value,indent=2)+'\n')
                return [first,second]
            with ThreadPoolExecutor(max_workers=args.jobs) as pool:
                for future in as_completed([pool.submit(pair,job) for job in jobs]):m['cases'].extend(future.result());save()
            m['boot_runtime']='PASS'
    except BaseException as e:m['boot_runtime']='FAIL';m['error']=repr(e);raise
    finally:
        m['changed_sources']=[p for p,h in base['sources'].items() if runner.H(runner.ROOT/p)!=h]
        m['changed_guards']=[p for p,h in base['guards'].items() if runner.H(runner.ROOT/p)!=h]
        m['changed_copies']=[p for p,h in base['compiled_inputs'].items() if runner.H(work/p)!=h]
        m['preservation']='FAIL' if m['changed_sources'] or m['changed_guards'] or m['changed_copies'] else 'PASS';save();print(out,m['boot_runtime'],m['preservation'],flush=True)
    assert m['preservation']=='PASS'
if __name__=='__main__':main()
