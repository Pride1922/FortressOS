"""Failure before actual root/login handoff must not fall back to live auth."""
import argparse,inspect,json,pathlib,shutil,subprocess
from concurrent.futures import ThreadPoolExecutor,as_completed
import test_pkg_boot_admission as runner
from pkg_enroll_fixture import gpt
from test_jbd2_replay_host import blocks

source=inspect.getsource(runner.boot).replace('def boot(','def refused_boot(')
source=source.replace("        if 'read-error' in label:","        if 'account-flush-loss' in label:argv+=['-fw_cfg','name=opt/fortress/pkg_account_flush_loss,string=private']\n        if 'read-error' in label:")
start=source.index("            wait(lambda s:'FortressOS login:");end=source.index('        finally:',start)
source=source[:start]+'''            wait(lambda s:'PKGROOT HALTED no-live-fallback' in s)
            s=text();assert 'PKGROOT PUBLISHED' not in s and 'FortressOS login:' not in s and not PROMPT.search(s)
            if 'account-flush-loss' in label:assert 'PKGROOT REFUSED stage=real account service' in s
            if 'account-flush-loss' not in label:assert 'PKGROOT REFUSED' in s or 'PKGBOOT REFUSED' in s
            p.terminate();p.wait(timeout=10)
            assert disk.read_bytes()[:2048*512]==source_bytes[:2048*512] and disk.read_bytes()[(2048+65536)*512:]==source_bytes[(2048+65536)*512:]
            unchanged=H(disk)==initial
            if 'selector' in label or 'bad-account' in label:assert unchanged
            root=dest/'root-after.img';root.write_bytes(disk.read_bytes()[2048*512:(2048+65536)*512])
            result=subprocess.run(['e2fsck','-fn',str(root)],capture_output=True,text=True);(dest/'linux-fsck.log').write_text(result.stdout+result.stderr);assert not result.returncode
            value={'boot_refusal':'PASS','root_published':False,'live_fallback':False,'whole_disk_unchanged':unchanged,'firmware':firmware,'smp':smp,'disk_sha256':H(disk)}
            (dest/'result.json').write_text(json.dumps(value,indent=2)+'\\n');print(label,'PASS refusal',flush=True);return value
''' +source[end:]
source=source.replace('initial=H(disk)','initial=H(disk);source_bytes=disk.read_bytes()')
scope=dict(vars(runner));exec(compile(source,'<running-root refusal>','exec'),scope);refused_boot=scope['refused_boot']

def main():
    parser=argparse.ArgumentParser();parser.add_argument('evidence');args=parser.parse_args();base=pathlib.Path(args.evidence).resolve();work=base/'workspace';out=base/'refusals';out.mkdir();source=base/'enrolled.img';raw=(base/'oracle-root.img').read_bytes();m={'boot_runtime':'INCOMPLETE','preservation':'INCOMPLETE','commands':[],'cases':[]}
    original=json.loads((base/'manifest.json').read_text());guards={p:runner.H(runner.ROOT/p) for p in original['sources']};guards.update({p:runner.H(runner.ROOT/p) for p in original['guards']})
    def save():(out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n')
    def run(cmd,label):
        m['commands'].append(cmd);save();r=subprocess.run(cmd,cwd=work,capture_output=True,text=True,timeout=180);(out/(label+'.log')).write_text(r.stdout+r.stderr);assert not r.returncode,(label,r.stdout[-1000:],r.stderr[-1000:])
    try:
        jobs=[]
        for label,path in (('bad-executable','/bin/login'),('bad-account','/etc/shadow'),('bad-home',None)):
            root=out/(label+'.root');root.write_bytes(raw)
            if path:
                data=bytearray(raw);data[blocks(root,path)[0]*4096]^=1;root.write_bytes(data)
            else:run(['debugfs','-w','-R','set_inode_field /home/operator mode 040755',str(root)],label)
            target=out/(label+'.img');target.write_bytes(gpt(root.read_bytes()));jobs.append((label,base/'valid.iso',target))
        for label in ('missing-selector','bad-selector'):
            modified=out/(label+'.module');target='/boot/selector.bin'
            if label=='missing-selector':
                text=(base/'iso-valid/boot/limine/limine.conf').read_text();modified.write_text(text.replace('    module_path: boot():/boot/selector.bin\n    module_cmdline: role=pkg-selector\n',''));target='/boot/limine/limine.conf'
            else:
                data=bytearray((base/'selector.bin').read_bytes());data[-1]^=1;modified.write_bytes(data)
            iso=out/(label+'.iso');run(['xorriso','-indev',str(base/'valid.iso'),'-outdev',str(iso),'-map',str(modified),target,'-boot_image','any','replay'],'iso-'+label);run([str(work/'limine/limine'),'bios-install',str(iso)],'bios-'+label);jobs.append((label,iso,source))
        jobs.append(('account-flush-loss',base/'valid.iso',source))
        with ThreadPoolExecutor(max_workers=4) as pool:
            futures=[pool.submit(refused_boot,out,work,iso,target,fw,smp,f'{label}-{fw}-{smp}') for label,iso,target in jobs for fw,smp in (('bios',1),('uefi',4))]
            for future in as_completed(futures):m['cases'].append(future.result());save()
        m['boot_runtime']='PASS'
    except BaseException as e:m['boot_runtime']='FAIL';m['error']=repr(e);raise
    finally:
        m['changed_inputs']=[p for p,h in guards.items() if runner.H(runner.ROOT/p)!=h];m['changed_copies']=[p for p,h in original['compiled_inputs'].items() if runner.H(work/p)!=h];m['preservation']='FAIL' if m['changed_inputs'] or m['changed_copies'] else 'PASS';save();print(out,m['boot_runtime'],m['preservation'],flush=True)
    assert m['preservation']=='PASS'
if __name__=='__main__':main()
