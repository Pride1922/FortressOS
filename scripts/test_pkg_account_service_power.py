"""Supplemental copied-probe pending publication I/O cuts and preview negatives.
Use an existing private service evidence directory; no production build targets.
"""
import argparse,datetime,hashlib,json,pathlib,re,struct,subprocess
from concurrent.futures import ThreadPoolExecutor
ROOT=pathlib.Path(__file__).resolve().parents[1]
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    ap=argparse.ArgumentParser();ap.add_argument('evidence',type=pathlib.Path);ap.add_argument('--smoke',action='store_true');args=ap.parse_args()
    base=args.evidence.resolve();m=json.loads((base/'manifest.json').read_text());work=base/'workspace'
    out=ROOT/'.codex-remote-attachments'/('pkg-account-service-power-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'));out.mkdir()
    result={'status':'INCOMPLETE','base':str(base),'commands':[],'cuts':[],'negatives':[],'guards':{p:sha(ROOT/p) for p in ('AGENTS.md','README.md','assets/ssl/ca-certificates.sha256')}}
    def run(cmd,label):
        result['commands'].append(cmd)
        with (out/(label+'.log')).open('w') as log:r=subprocess.run(cmd,cwd=work,stdout=log,stderr=subprocess.STDOUT,timeout=240)
        text=(out/(label+'.log')).read_text();assert not r.returncode,(label,r.returncode,text[-2000:]);return text
    source=ROOT/'tests/pkg_account_service_durable.inc';text=source.read_text();result['service_source']=sha(source)
    text='static bool sv_power;static long sv_power_cut;static unsigned sv_power_profile;\n'+text
    old='    int r=sv_backend.rename(p,from,to);sv_pending_move=false;return r;'
    assert text.count(old)==1
    text=text.replace(old,'''    if(sv_power&&sv_pending_move){ad_disk->events=ad_disk->writes=ad_disk->flushes=0;ad_disk->cut=sv_power_cut;ad_disk->persistence=sv_power_profile/2;ad_disk->after=sv_power_profile%2;}
    int r=sv_backend.rename(p,from,to);
    if(sv_power&&sv_pending_move){printf("POWER events=%zu error=%d\\n",ad_disk->events,r);ad_disk->cut=-1;}
    sv_pending_move=false;return r;''')
    text=text.replace('        sv_cut=cancel?', '''        sv_power=!strncmp(argv[1],"power=",6);
        if(sv_power){assert(sscanf(argv[1]+6,"%u:%ld",&sv_power_profile,&sv_power_cut)==2&&sv_power_profile<8);cancel=false;}
        sv_cut=cancel?''')
    text=text.replace('            if(cancel){assert(result==SYSCALL_EINTR', '''            if(sv_power&&result){assert(result==SYSCALL_EIO&&!enabled&&d.offline);size_t ev=d.events;accounts_process_exit(77);assert(!enabled&&!writer&&d.events==ev);save(prefix,"interrupted",d.stable,size);puts("PASS power auth=blocked");}
            else if(cancel){assert(result==SYSCALL_EINTR''')
    inc=out/'service-power.inc';inc.write_text(text)
    tu=(work/'tests/account-service-durable.c').read_text().replace('#include "pkg_account_service_durable.inc"',f'#include "{inc}"')
    generated=out/'service-power.c';generated.write_text(tu)
    compile_cmd=next(c for c in m['commands'] if c[-2:]==['-o',str(base/'probe')])
    compile_cmd=[str(generated) if x=='tests/account-service-durable.c' else str(out/'probe') if x==str(base/'probe') else x for x in compile_cmd]
    try:
        run(compile_cmd,'compile')
        for ss in (512,4096):
            tag=f'4096-{ss}';setup=base/(tag+'-setup.img');en=base/(tag+'-enrollment.bin');pol=base/(tag+'-policy.bin')
            def probe(mode,image,prefix):return run([str(out/'probe'),mode,str(image),str(ss),str(en),str(pol),str(prefix)],prefix.name)
            prefix=out/(tag+'-baseline');normal=probe('power=0:-1',setup,prefix);events=int(re.search(r'POWER events=(\d+) error=0',normal)[1])
            result.setdefault('event_counts',{})[tag]=events
            def cut_case(pair):
                profile,cut=pair;prefix=out/(tag+f'-p{profile}-c{cut}')
                log=probe(f'power={profile}:{cut}',setup,prefix);assert 'PASS power auth=blocked' in log
                image=pathlib.Path(str(prefix)+'-interrupted.img');initial=sha(image);reports=[]
                for n in range(2):
                    pre=out/(prefix.name+f'-reboot{n}');reports.append(probe('recover',image,pre));assert 'PASS family=' in reports[-1]
                    final=pathlib.Path(str(pre)+'-final.img');run(['e2fsck','-fn',str(final)],pre.name+'-fsck')
                    if n==1:assert 'commits=0' in reports[-1]
                    image.unlink();image=final
                image.unlink();return {'profile':profile,'cut':cut,'interrupted_sha256':initial,'reports':reports}
            pairs=[(p,c) for p in range(8) for c in (range(events) if not args.smoke else sorted({0,events//2,events-1}))]
            with ThreadPoolExecutor(max_workers=8) as pool:result['cuts']+=list(pool.map(cut_case,pairs))
            # Re-seal the state so these isolate semantic validation, not a hash check.
            body=(base/(tag+'-body.img')).read_bytes();state_at=struct.unpack_from('<Q',body,416+5*128+48)[0]*4096
            raw=setup.read_bytes()
            for label,offset,value in [('zero-auth',144,0),('unissued-target',152,2),('phase-without-target',160,1),('wrong-old-length',424,1)]:
                data=bytearray(raw);state=bytearray(data[state_at:state_at+1024]);struct.pack_into('<H' if offset==160 else '<I' if offset==424 else '<Q',state,offset,value);state[992:]=hashlib.sha256(state[:992]).digest();data[state_at:state_at+1024]=state
                bad=out/(tag+'-'+label+'.img');bad.write_bytes(data);before=sha(bad);pre=out/(tag+'-'+label)
                assert 'PASS reject writes=0 flushes=0' in probe('reject',bad,pre) and sha(bad)==before
                result['negatives'].append(tag+'-'+label);bad.unlink()
            pre=out/(tag+'-pending-seed');pending_log=probe('update=10',setup,pre)
            assert 'phase=1' in pending_log
            pending=pathlib.Path(str(pre)+'-interrupted.img');pending_raw=pending.read_bytes()
            def block(path):
                text=run(['debugfs','-R','blocks '+path,str(pending)],tag+'-blocks-'+path.rsplit('/',1)[-1])
                return int(re.search(r'(?m)^([0-9]+)',text)[1])*4096
            pending_at=block('/var/lib/fortress/accounts/pending');stage_at=block('/var/lib/fortress/accounts/passwd.new');canonical_at=block('/etc/passwd')
            for label,kind in [('pending-generation',0),('planned-generation',1),('stage-hash',2),('canonical-hash',3),('pending-phase',4),('missing-stage',5)]:
                data=bytearray(pending_raw);state=bytearray(data[state_at:state_at+1024])
                if kind==0:
                    record=bytearray(data[pending_at:pending_at+160]);struct.pack_into('<Q',record,8,3);record[128:]=hashlib.sha256(record[:128]).digest();data[pending_at:pending_at+160]=record
                elif kind==1:
                    struct.pack_into('<QQ',state,144,3,3);struct.pack_into('<Q',state,176,3);state[296:328]=hashlib.sha256(state[168:296]).digest()
                elif kind==2:data[stage_at+1]^=1
                elif kind==3:data[canonical_at+1]^=1
                elif kind==4:struct.pack_into('<H',state,160,2)
                state[992:]=hashlib.sha256(state[:992]).digest();data[state_at:state_at+1024]=state
                bad=out/(tag+'-'+label+'.img');bad.write_bytes(data)
                if kind==5:run(['debugfs','-w','-R','rm /var/lib/fortress/accounts/passwd.new',str(bad)],tag+'-remove-stage')
                before=sha(bad);pre=out/(tag+'-'+label)
                assert 'PASS reject writes=0 flushes=0' in probe('reject',bad,pre) and sha(bad)==before
                result['negatives'].append(tag+'-'+label);bad.unlink()
            pending.unlink()
            print(tag,events,len(pairs),'PASS',flush=True)
        result['status']='PASS'
    except Exception as e:result.update(status='FAIL',error=repr(e));raise
    finally:
        result['source_preserved']=sha(source)==result['service_source']
        result['base_compiled_copies_preserved']=all(sha(work/p)==h for p,h in m['compiled_inputs'].items())
        result['guards_after']={p:sha(ROOT/p) for p in result['guards']}
        if not result['source_preserved'] or not result['base_compiled_copies_preserved'] or result['guards']!=result['guards_after']:result['status']='FAIL'
        result['artifacts']={p.name:sha(p) for p in out.iterdir() if p.is_file()}
        (out/'manifest.json').write_text(json.dumps(result,indent=2)+'\n');print('Evidence:',out,result['status'],flush=True)
    assert result['status']=='PASS'
if __name__=='__main__':main()
