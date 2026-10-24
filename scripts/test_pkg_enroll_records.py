"""Fresh actual-filesystem TEST enrollment, native readback and publication cuts."""
import datetime,hashlib,json,os,pathlib,shutil,struct,subprocess,tempfile
from pkg_enroll_fixture import create,gpt,BS,FILES
from pkg_enroll_records import build,write_fresh_records
from pkg_test_enroll import publish,NAMES
from pkg_test_enroll_read import inspect
ROOT=pathlib.Path(__file__).resolve().parents[1]
H=lambda b:hashlib.sha256(b).digest()

def main():
    out=ROOT/'.codex-remote-attachments'/('pkg-enroll-records-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
    out.mkdir();work=out/'workspace';work.mkdir()
    m={'result':'INCOMPLETE','commands':[],'cases':[],'root_cuts':[],'publication_cuts':[],
       'scope':'fresh TEST-only EXT4/GPT records and host actual-code readback; no kernel boot or production provisioning'}
    sha=lambda p:H(p.read_bytes()).hex()
    guards={str(p.relative_to(ROOT)):sha(p) for name in ('src','user','scripts','tests','docs') for p in (ROOT/name).rglob('*') if p.is_file() and '__pycache__' not in p.parts}
    guards.update({str(p.relative_to(ROOT)):sha(p) for p in [*(ROOT/'bin').glob('*'),ROOT/'Makefile',ROOT/'PROTECTED.md',ROOT/'AGENTS.md',ROOT/'assets/ssl/ca-certificates.sha256'] if p.is_file()})
    m['guards']=guards
    def save():(out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n')
    def run(cmd,label,cwd=work):
        m['commands'].append(cmd);save();p=subprocess.run(cmd,cwd=cwd,capture_output=True,text=True,timeout=180)
        (out/(label+'.log')).write_text(p.stdout+p.stderr)
        if p.returncode:raise RuntimeError((label,p.returncode,p.stdout[-2000:],p.stderr[-2000:]))
        return p.stdout
    flags=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-pthread','-DTOOL_HOST_TEST','-DFORTRESS_DAC_ENFORCED','-I.','-Itests','-Itests/ext4_host','-Itests/host','-Isrc/include','-Isrc/fs','-Isrc/drivers','-Isrc/mm','-Isrc/lib','-Isrc/kernel','-Iuser/tools','-Isrc/lib/crypto/upstream/src']
    native_units=['tests/pkg_enroll_geometry_host.c','tests/ext4_fault_disk.c','src/fs/permission_values.c','src/kernel/creds.c','src/lib/account_redo.c','user/tools/digest.c']
    selector_units=['tests/pkg_selector_candidate.c','tests/pkg_selector_host.c','tests/pkg_enrollment_candidate.c','tests/pkg_grant_candidate.c','tests/pkg_mount_candidate.c','user/pkg/catalog.c','user/pkg/verify.c','user/pkg/format.c','user/pkg/manifest.c','user/tools/gzip.c','user/tools/common.c','user/tools/digest.c','src/lib/crypto/hash.c','src/lib/crypto/curve.c']
    mono=['src/lib/crypto/upstream/src/monocypher.c','src/lib/crypto/upstream/src/optional/monocypher-ed25519.c']
    try:
        deps=run(flags+['-MM',*sorted(set(native_units+selector_units))],'dependencies',ROOT)
        deps+=run(flags+['-include','src/lib/crypto/namespace.h','-MM',*mono],'crypto-dependencies',ROOT)
        names={os.path.normpath(p) for p in deps.replace('\\\n',' ').split() if not p.endswith(':')}
        for name in names:
            source=ROOT/name;assert source.resolve().is_relative_to(ROOT)
            dest=work/name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,dest)
        m['compiled_inputs']={name:sha(work/name) for name in sorted(names)}
        native=out/'native-reader';run(flags+native_units+['-o',str(native)],'compile-native')
        objs=[]
        for i,p in enumerate(mono):
            obj=out/f'mono-{i}.o';run(flags+['-include','src/lib/crypto/namespace.h','-c',p,'-o',str(obj)],f'compile-crypto-{i}');objs.append(str(obj))
        verifier=out/'selector-reader';run(flags+selector_units+objs+['-o',str(verifier)],'compile-selector')
        m['binaries']={p.name:sha(p) for p in (native,verifier)}
        with tempfile.TemporaryDirectory(prefix='fortress-record-enroll-') as temp:
            temp=pathlib.Path(temp);seed_roles={}
            for name,values in (('selector',(111,112,113)),('enrollment',(41,42,43)),('catalog',(1,2,3))):
                seed_roles[name]=[]
                for value in values:
                    p=temp/f'{name}-{value}.seed';p.write_bytes(bytes([value])*32);p.chmod(0o600);seed_roles[name].append(p)
            base=temp/'base';base.mkdir();image,_=create(base,identities=True);initial=image.read_bytes()
            prepared=build(image,temp/'records',seed_roles,H(b'host TEST kernel identity, not boot measurement'),H(b'host TEST bootstrap identity, not boot measurement'))
            for name,key in (('candidate-selector.bin','selector'),('trusted-policy.bin','policy'),('expected-bundle.bin','bundle'),('identities.bin','witness'),('root-authority.json','root_authority')):(out/name).write_bytes(prepared[key])
            (out/'identities.json').write_text(json.dumps(prepared['identities'],indent=2)+'\n')
            policy=out/'trusted-policy.bin';selector=out/'candidate-selector.bin';witness=out/'identities.bin'
            run([str(verifier),str(policy),str(selector),str(out/'expected-bundle.bin'),'1'],'prepared-cryptographic-check')
            events=[];write_fresh_records(image,prepared,events.append);final=image.read_bytes()
            assert len(events)==9 and events[-1]=='root-fsynced'
            # Independent expected image: every byte outside logical record
            # content, including padding and all metadata, must remain exact.
            expected=bytearray(initial)
            for part,index in zip(prepared['parts'],(16,0,1,2,5,4,6,7)):
                at=0
                for lo,n,physical in prepared['identities'][index]['extents']:
                    take=min(n*BS,len(part)-at);assert lo*BS==at
                    expected[physical*BS:physical*BS+take]=part[at:at+take];at+=take
                assert at==len(part)
            assert final==expected;m['non_record_bytes_preserved']=True
            disk=out/'enrolled.img';disk.write_bytes(gpt(final));before=sha(disk)
            bundle=out/'native-bundle.bin';run([str(native),str(disk),'accept',str(witness),str(bundle)],'native-readback')
            assert bundle.read_bytes()==prepared['bundle'] and sha(disk)==before
            # Signed locators must be the very native identity expectations
            # just checked, not an unrelated unsigned geometry observation.
            assert prepared['parts'][0][416:2464]==prepared['witness'][32:32+16*128]
            run([str(verifier),str(policy),str(selector),str(bundle),'1'],'native-cryptographic-check')
            run(['e2fsck','-fn',str(image)],'linux-fsck')
            for name,path,part in zip(NAMES,('/pkg/descriptor.v2','/pkg/enrollment.v1','/pkg/grant.v1','/pkg/ownership.v1','/pkg/account-generation.v1','/pkg/origin.v1','/pkg/state.v1','/pkg/trust.v1'),prepared['parts']):
                dump=temp/('dump-'+name);run(['debugfs','-R',f'dump {path} {dump}',str(image)],'linux-'+name);assert dump.read_bytes()==part
            for path,content in FILES.items():
                dump=temp/('dump-'+path.rsplit('/',1)[1]);run(['debugfs','-R',f'dump {path} {dump}',str(image)],'linux-'+path.rsplit('/',1)[1]);assert dump.read_bytes()==content
            for i,index in enumerate((16,0,1,2,5,4,6,7,17,18,19)):
                bad=bytearray(final);physical=prepared['identities'][index]['extents'][0][2];bad[physical*BS]^=1
                p=out/f'corrupt-{i}.img';p.write_bytes(gpt(bad));old=sha(p);bf=out/f'corrupt-{i}.bundle'
                expected_native='accept' if 0<i<8 else 'refuse'
                run([str(native),str(p),expected_native,str(witness),str(bf)],f'corrupt-{i}-native')
                if 0<i<8:run([str(verifier),str(policy),str(selector),str(bf),'0'],f'corrupt-{i}-crypto')
                else:assert not bf.exists()
                assert sha(p)==old;m['cases'].append({'case':'corrupt-'+str(i),'refused':True,'disk_unchanged':True})
            for cut in range(1,10):
                p=temp/f'cut-{cut}.img';p.write_bytes(initial);seen=[]
                def event(label):
                    seen.append(label)
                    if len(seen)==cut:raise InterruptedError(label)
                try:write_fresh_records(p,prepared,event)
                except InterruptedError:pass
                else:raise AssertionError('root interruption not reached')
                cutdisk=out/f'root-cut-{cut}.img';cutdisk.write_bytes(gpt(p.read_bytes()));old=sha(cutdisk)
                cutbundle=out/f'root-cut-{cut}.bundle'
                run([str(native),str(cutdisk),'accept' if cut>=5 else 'refuse',str(witness),str(cutbundle)],f'root-cut-{cut}-native')
                if cut>=5:run([str(verifier),str(policy),str(selector),str(cutbundle),'1' if cut>=8 else '0'],f'root-cut-{cut}-crypto')
                assert sha(cutdisk)==old
                # No selector exists during any root write, including fully
                # durable records before the subsequent readback check.
                boot=temp/f'root-cut-{cut}';boot.mkdir(mode=0o700)
                assert inspect(boot,True,verifier,policy)=='REFUSE'
                m['root_cuts'].append({'cut':cut,'event':seen[-1],'admission':'REFUSE','selector_absent':True})
            result=publish(prepared['selector'],bundle.read_bytes(),prepared['audit'],temp/'published',verifier,policy)
            assert inspect(temp/'published',True,verifier,policy)=='PROTECTED'
            for cut in range(1,len(result['events'])+1):
                target=temp/f'publication-cut-{cut}'
                try:publish(prepared['selector'],bundle.read_bytes(),prepared['audit'],target,verifier,policy,cut)
                except InterruptedError:pass
                else:raise AssertionError('publication interruption not reached')
                status=inspect(target,True,verifier,policy)
                committed=(target/'boot/selector.bin').exists()
                assert status==('PROTECTED' if committed else 'REFUSE')
                m['publication_cuts'].append({'cut':cut,'event':result['events'][cut-1],'admission':status})
            assert sha(disk)==before
            m['publication_events']=result['events'];m['enrolled_disk_sha256']=before
            m['root_events']=events;m['runtime_result']='PASS';m['boot_acceptance']='NOT RUN'
    except Exception as e:m['error']=repr(e);m['result']='FAIL';raise
    finally:
        changed=[p for p,h in guards.items() if not (ROOT/p).is_file() or sha(ROOT/p)!=h]
        copies=[p for p,h in m.get('compiled_inputs',{}).items() if sha(work/p)!=h]
        m['changed_inputs']=changed;m['changed_compiled_copies']=copies
        m['preservation_result']='FAIL' if changed or copies else 'PASS'
        if m.get('runtime_result')=='PASS':m['result']='FAIL' if changed or copies else 'PASS'
        save();print(m['result'],out,flush=True)

if __name__=='__main__':main()
