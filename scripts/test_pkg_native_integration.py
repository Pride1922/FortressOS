"""Native owner failures + package-gated detached root/account service."""
import argparse,inspect,json,pathlib,re,subprocess,sys
from concurrent.futures import ThreadPoolExecutor,as_completed
import test_pkg_boot_admission as runner
from pkg_pending_records import prepare
from pkg_native_integration_overlay import overlay

def build(image,out,roles,kernel,bootstrap):return prepare(image,pathlib.Path(out),roles,kernel,bootstrap)

source=inspect.getsource(runner.boot).replace('def boot(','def integration_boot(')
source=source.replace('sequence=1):','sequence=1,expected_counts=None,fault=False):')
source=source.replace("f'PKGBOOT ADMITTED IDLE={sequence} detached'","'PKGBOOT ADMITTED RECOVERY detached'")
source=source.replace("uart.sendall(b'sudo /bin/pkg-boot refused\\n' if refused else b'sudo /bin/pkg-boot\\n')","uart.sendall(b'sudo /bin/pkg-boot bad-home\\n' if 'bad-home' in label else b'sudo /bin/pkg-boot sync-loss\\n' if 'sync-loss' in label and not label.endswith('-reboot') else b'sudo /bin/pkg-boot fault\\n' if fault else b'sudo /bin/pkg-boot\\n')")
source=source.replace('assert H(disk)==(H(expected) if expected else initial)','assert fault or H(disk)==(H(expected) if expected else initial)')
source=source.replace('((16,2) if expected else (0,0))','(expected_counts if expected_counts is not None else (0,0))')
scope=dict(vars(runner));exec(compile(source,'<native package integration runner>','exec'),scope);integration_boot=scope['integration_boot']

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--native',required=True);parser.add_argument('--reuse-build');parser.add_argument('--smoke',action='store_true');args=parser.parse_args()
    native=pathlib.Path(args.native).resolve();assert native.is_file()
    if args.reuse_build:out=pathlib.Path(args.reuse_build).resolve()
    else:
        runner.overlay=overlay;runner.build=build;before=set((runner.ROOT/'.codex-remote-attachments').glob('pkg-boot-admission-*'));saved=sys.argv;sys.argv=[saved[0],'--build-only']
        try:runner.main()
        finally:sys.argv=saved
        added=set((runner.ROOT/'.codex-remote-attachments').glob('pkg-boot-admission-*'))-before;assert len(added)==1;out=added.pop()
    base=json.loads((out/'manifest.json').read_text());work=out/'workspace';assert not base.get('error') and not base['changed_copies'];assert all(runner.H(work/p)==h for p,h in base['compiled_inputs'].items())
    for name,path in (('kernel',work/'bin/fortress.elf'),('bootstrap',out/'bootstrap.tar'),('selector',out/'selector.bin'),('policy',out/'policy.bin')):assert runner.H(path)==base['measurements'][name]
    assert not (out/'integration-manifest.json').exists()
    m={'boot_runtime':'INCOMPLETE','preservation':'INCOMPLETE','cases':[],'commands':[],'base_build_preservation':base['preservation'],'scope':'private detached VFS root, isolated actual accounts service, no public root publication'}
    def save():(out/'integration-manifest.json').write_text(json.dumps(m,indent=2)+'\n')
    try:
        # Read-only regression/reference binary from the retained signed corpus;
        # root/account lookup adds no mutations beyond one validated sync.
        expected=out/'integration-expected.img';witness=out/'integration.witness'
        from pkg_enroll_identity import inventory
        # Reconstruct the complete witness from the actual prepared fixture.
        wire,_=inventory(out/'oracle-root.img');witness.write_bytes(wire)
        cmd=[str(native),str(out/'enrolled.img'),str(out/'policy.bin'),str(out/'selector.bin'),str(witness),str(expected),'-1','1'];m['commands'].append(cmd);save();r=subprocess.run(cmd,cwd=work,capture_output=True,text=True,timeout=240);(out/'integration-native.log').write_text(r.stdout+r.stderr);assert not r.returncode,(r.stdout,r.stderr)
        counts=tuple(map(int,re.search(r'PASS native recovery writes=(\d+) flushes=(\d+)',r.stdout).groups()));counts=(counts[0],counts[1]+1)
        badroot=out/'bad-home.root';badroot.write_bytes((out/'oracle-root.img').read_bytes());cmd=['debugfs','-w','-R','set_inode_field /root mode 040755',str(badroot)];m['commands'].append(cmd);r=subprocess.run(cmd,capture_output=True,text=True);(out/'bad-home-prepare.log').write_text(r.stdout+r.stderr);assert not r.returncode
        from pkg_enroll_fixture import gpt
        badsource=out/'bad-home.img';badsource.write_bytes(gpt(badroot.read_bytes()));badexpected=out/'bad-home-expected.img';cmd=[str(native),str(badsource),str(out/'policy.bin'),str(out/'selector.bin'),str(witness),str(badexpected),'-1','1'];m['commands'].append(cmd);r=subprocess.run(cmd,cwd=work,capture_output=True,text=True,timeout=240);(out/'bad-home-native.log').write_text(r.stdout+r.stderr);assert not r.returncode
        configs=[('bios',1)] if args.smoke else [('bios',1),('bios',4),('uefi',1),('uefi',4)]
        def campaign(config):
            fw,smp=config;label=f'integration-{fw}-{smp}';values=[]
            values.append(integration_boot(out,work,out/'valid.iso',out/'enrolled.img',fw,smp,label,expected=expected,expected_counts=counts))
            values.append(integration_boot(out,work,out/'valid.iso',out/label/'enrolled.img',fw,smp,label+'-reboot',expected=out/label/'enrolled.img',expected_counts=(0,1)))
            fault=label+'-ack-loss';values.append(integration_boot(out,work,out/'valid.iso',out/'enrolled.img',fw,smp,fault,fault=True,expected_counts=(1,0)))
            # Failed first sector acknowledgement may alter only journal bytes.
            from test_jbd2_replay_host import blocks
            journal=blocks(out/'oracle-root.img','<8>');original=bytearray((out/'enrolled.img').read_bytes());failed=bytearray((out/fault/'enrolled.img').read_bytes())
            for block in journal:
                at=2048*512+4096*block;original[at:at+4096]=failed[at:at+4096]=bytes(4096)
            assert original==failed,'uncertain acknowledgement changed non-journal storage'
            values.append(integration_boot(out,work,out/'valid.iso',out/fault/'enrolled.img',fw,smp,fault+'-reboot',expected=expected,expected_counts=counts))
            bad=label+'-bad-home';values.append(integration_boot(out,work,out/'valid.iso',badsource,fw,smp,bad,expected=badexpected,expected_counts=(counts[0],counts[1]-1)))
            values.append(integration_boot(out,work,out/'valid.iso',out/bad/'enrolled.img',fw,smp,bad+'-reboot',expected=out/bad/'enrolled.img',expected_counts=(0,0)))
            sync=label+'-sync-loss';values.append(integration_boot(out,work,out/'valid.iso',out/'enrolled.img',fw,smp,sync,expected=expected,expected_counts=counts))
            values.append(integration_boot(out,work,out/'valid.iso',out/sync/'enrolled.img',fw,smp,sync+'-reboot',expected=out/sync/'enrolled.img',expected_counts=(0,1)))
            return values
        with ThreadPoolExecutor(max_workers=len(configs)) as pool:
            for future in as_completed([pool.submit(campaign,c) for c in configs]):m['cases'].extend(future.result());save()
        frames=[]
        for line in (work/'build/fs/ext4.su').read_text().splitlines():
            f=line.split('\t');name=f[0].split(':')[-1]
            if name.startswith(('jn_','jb_','pn_')):assert int(f[1])<=1024,(name,f);frames.append({'function':name,'bytes':int(f[1])})
        m['private_fs_frames']=frames;m['boot_runtime']='PASS'
    except BaseException as e:m['boot_runtime']='FAIL';m['error']=repr(e);raise
    finally:
        m['changed_sources']=[p for p,h in base['sources'].items() if runner.H(runner.ROOT/p)!=h];m['changed_guards']=[p for p,h in base['guards'].items() if runner.H(runner.ROOT/p)!=h];m['changed_copies']=[p for p,h in base['compiled_inputs'].items() if runner.H(work/p)!=h]
        m['preservation']='FAIL' if m['changed_sources'] or m['changed_guards'] or m['changed_copies'] else 'PASS';save();print(out,m['boot_runtime'],m['preservation'],flush=True)
    assert m['preservation']=='PASS'
if __name__=='__main__':main()
