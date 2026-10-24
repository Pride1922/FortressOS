"""Protected-checkpoint advancement, rollback and incompatible-kernel boots."""
import argparse,datetime,json,pathlib,struct,subprocess,tempfile
from concurrent.futures import ThreadPoolExecutor,as_completed
import test_pkg_boot_admission as runner
from test_pkg_running_root import installed_boot
from test_pkg_running_root_refusal import refused_boot
from pkg_trust_records import verify_readback_and_raise
from pkg_enroll_records import H,keys,signed
from pkg_enroll_fixture import gpt
def main():
    parser=argparse.ArgumentParser();parser.add_argument('evidence');parser.add_argument('--old-kernel',required=True);args=parser.parse_args();base=pathlib.Path(args.evidence).resolve();work=base/'workspace';out=base/('trust-acceptance-'+datetime.datetime.now().strftime('%H%M%S'));out.mkdir()
    original=json.loads((base/'manifest.json').read_text());guards={p:runner.H(runner.ROOT/p) for p in original['sources']};guards.update({p:runner.H(runner.ROOT/p) for p in original['guards']})
    m={'boot_runtime':'INCOMPLETE','preservation':'INCOMPLETE','commands':[],'cases':[],'boundary':'new trusted boot policy outside root disk; rollback of both policy and root is excluded'}
    def save():(out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n')
    def run(cmd,label):
        m['commands'].append(cmd);save();r=subprocess.run(cmd,cwd=work,capture_output=True,text=True,timeout=180);(out/(label+'.log')).write_text(r.stdout+r.stderr);assert not r.returncode,(label,r.stderr[-2000:])
    def iso(label,module,target='/boot/policy.bin'):
        path=out/(label+'.module');path.write_bytes(module);dest=out/(label+'.iso');run(['xorriso','-indev',str(base/'valid.iso'),'-outdev',str(dest),'-map',str(path),target,'-boot_image','any','replay'],'iso-'+label);run([str(work/'limine/limine'),'bios-install',str(dest)],'bios-'+label);return dest
    try:
        root=base/'running-bios-1-reboot/root-after.img';policy=(base/'policy.bin').read_bytes();strong=verify_readback_and_raise(root,policy);latest=iso('minimum3',strong)
        # Refuse premature publication and any readback mismatching the signed end.
        for label,path in (('unadvanced',base/'oracle-root.img'),):
            try:verify_readback_and_raise(path,policy)
            except (AssertionError,ValueError):m['cases'].append({'readback_refusal':label,'result':'PASS'})
            else:raise AssertionError('premature checkpoint publication')
        oldroot=bytearray((base/'oracle-root.img').read_bytes());offset=1152+struct.unpack_from('<I',strong,1168)[0];end=strong[offset+2048:offset+3072];physical=struct.unpack_from('<Q',end,528)[0];oldroot[physical*4096:physical*4096+1024]=end
        partial=out/'partial-rollback.img';partial.write_bytes(gpt(oldroot));damaged=bytearray(root.read_bytes());damaged[physical*4096+700]^=1;bad=out/'bad-floor.img';bad.write_bytes(gpt(damaged))
        jobs=[('whole-disk-rollback',latest,base/'enrolled.img'),('partial-rollback',latest,partial),('bad-floor',latest,bad)]
        with tempfile.TemporaryDirectory(prefix='fortress-trust-accept-') as td:
            paths=[]
            for v in (111,112,113):p=pathlib.Path(td)/str(v);p.write_bytes(bytes([v])*32);p.chmod(0o600);paths.append(p)
            signers=keys(paths)
            for label,position,value in (('future-contract',32,struct.pack('<I',4)),('wrong-kernel',152,H(b'incompatible kernel')),('wrong-bootstrap',184,H(b'incompatible bootstrap')),('equal-version-other-hash',256,H(b'root equivocation'))):
                altered=bytearray(strong)
                for i in (1,2):
                    at=offset+i*1024;record=bytearray(altered[at:at+640]);record[position:position+len(value)]=value;record[608:640]=H(record[:608]);altered[at:at+1024]=signed(record,signers).ljust(1024,b'\0')
                jobs.append((label,iso(label,altered),base/'running-bios-1-reboot/enrolled.img'))
        oldkernel=pathlib.Path(args.old_kernel).resolve();assert oldkernel.is_file() and not oldkernel.is_symlink();oldiso=iso('old-kernel',oldkernel.read_bytes(),'/boot/fortress.elf');m['old_kernel_sha256']=runner.H(oldkernel)
        # Derive the legacy refusal runner from its retained generated source.
        import test_pkg_running_root_refusal as refusal
        source=refusal.source.replace('def refused_boot(','def old_boot(').replace("'PKGROOT HALTED no-live-fallback' in s","'[ROOT RECOVERY] invalid boot selection' in s").replace("if 'account-flush-loss' not in label:assert 'PKGROOT REFUSED' in s or 'PKGBOOT REFUSED' in s","assert 'PKGBOOT REFUSED stage=module-admission' in s")
        scope=dict(vars(runner));exec(compile(source,'<old incompatible kernel>','exec'),scope);old_boot=scope['old_boot']
        def refuse(job,fw,smp):
            label,medium,target=job;result=refused_boot(out,work,medium,target,fw,smp,f'{label}-{fw}-{smp}');assert result['whole_disk_unchanged'];result['case']=label;return result
        with ThreadPoolExecutor(max_workers=4) as pool:
            tasks=[pool.submit(refuse,j,fw,smp) for j in jobs for fw,smp in (('bios',1),('uefi',4))]
            for future in as_completed(tasks):m['cases'].append(future.result());save()
        for fw,smp in (('bios',1),('uefi',4)):
            label=f'old-kernel-{fw}-{smp}';result=old_boot(out,work,oldiso,base/'running-bios-1-reboot/enrolled.img',fw,smp,label);assert result['whole_disk_unchanged'];result['case']='old-kernel';m['cases'].append(result);save()
        def pair(config):
            fw,smp=config;label=f'checkpoint3-{fw}-{smp}';a=installed_boot(out,work,latest,base/'running-bios-1-reboot/enrolled.img',fw,smp,label);b=installed_boot(out,work,latest,out/label/'enrolled.img',fw,smp,label+'-reboot');return [a,b]
        with ThreadPoolExecutor(max_workers=4) as pool:
            for future in as_completed([pool.submit(pair,c) for c in (('bios',1),('bios',4),('uefi',1),('uefi',4))]):m['cases'].extend(future.result());save()
        m['boot_runtime']='PASS'
    except BaseException as e:m['boot_runtime']='FAIL';m['error']=repr(e);raise
    finally:
        m['changed_inputs']=[p for p,h in guards.items() if runner.H(runner.ROOT/p)!=h];m['changed_copies']=[p for p,h in original['compiled_inputs'].items() if runner.H(work/p)!=h];m['preservation']='FAIL' if m['changed_inputs'] or m['changed_copies'] else 'PASS';save();print(out,m['boot_runtime'],m['preservation'],flush=True)
    assert m['preservation']=='PASS'
if __name__=='__main__':main()
