"""Running enrolled EXT4 root, real installed login/account service and reboot."""
import argparse,inspect,json,pathlib,re,struct,subprocess,sys,tarfile
from concurrent.futures import ThreadPoolExecutor,as_completed
import test_pkg_boot_admission as runner
import pkg_running_root_records as records
from pkg_running_root_overlay import overlay
from pkg_enroll_fixture import FILES,gpt
PASSWORD='GateOne-Only-42'

def apply(work,replace):records.BUILD_BOOTSTRAP=work.parent/'bootstrap.tar';return overlay(work,replace)

source=inspect.getsource(runner.boot).replace('def boot(','def installed_boot(')
source=source.replace("'PKGBOOT RING3 FAIL'","'PKGROOT RING3 FAIL'")
source=source.replace("f'PKGBOOT ADMITTED IDLE={sequence} detached'","'PKGROOT PUBLISHED real-account-service installed-login'")
start=source.index('            if not PROMPT.search(s):');end=source.index('            if not refused:',start)
source=source[:start]+'''            assert not PROMPT.search(s),'unexpected passwordless shell'
            uart.sendall(b'operator\\n');wait(lambda s:'Password: ' in s)
            uart.sendall(b'wrong-gate-password\\n');wait(lambda s:'Login incorrect' in s and s.rfind('FortressOS login: ')>s.rfind('Login incorrect'))
            uart.sendall(b'operator\\n');wait(lambda s:s.rfind('Password: ')>s.rfind('Login incorrect'))
            uart.sendall(PASSWORD.encode()+b'\\n');wait(lambda s:PROMPT.search(s))
            assert PASSWORD not in text() and 'passwordless' not in text()
            begin=len(text());uart.sendall(b'/bin/pkg-boot\\n');wait(lambda s:'PKGROOT USER PASS' in s[begin:] and PROMPT.search(s[begin:]))
            begin=len(text());uart.sendall(b'sudo /bin/pkg-boot\\n');wait(lambda s:'sudo password: ' in s[begin:]);uart.sendall(PASSWORD.encode()+b'\\n');wait(lambda s:'PKGROOT ROOT PASS' in s[begin:] and PROMPT.search(s[begin:]))
            assert PASSWORD not in text()
            begin=len(text());uart.sendall(b'sudo /bin/pkg-boot power\\n');wait(lambda s:'sudo password: ' in s[begin:]);uart.sendall(PASSWORD.encode()+b'\\n');p.wait(timeout=30);assert not p.returncode
            raw=disk.read_bytes();before=source_bytes
            assert raw[:2048*512]==before[:2048*512] and raw[(2048+65536)*512:]==before[(2048+65536)*512:]
''' +source[end:]
source=source.replace('initial=H(disk)','initial=H(disk);source_bytes=disk.read_bytes()')
source=source.replace("    dest=out/label;", "    source_path=source;dest=out/label;")
source=source.replace("            result={'firmware'","            result={'firmware'")
# Old counter variables describe private recover-only probes, not this installed
# session. This runner instead audits actual filesystem records and Linux data.
source=source.replace("'writes':writes,'flushes':flushes,","'writes':None,'flushes':None,")
scope=dict(vars(runner),PASSWORD=PASSWORD);exec(compile(source,'<running-root boot>','exec'),scope);installed_boot=scope['installed_boot']

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--smoke',action='store_true');parser.add_argument('--reuse-build');args=parser.parse_args();saved_files=dict(FILES)
    if args.reuse_build:out=pathlib.Path(args.reuse_build).resolve()
    else:
        hashed=subprocess.check_output(['openssl','passwd','-5','-salt','GateOne42',PASSWORD],text=True).strip()
        FILES.update({'/etc/passwd':b'root:x:0:0:root:/root:/bin/shell\noperator:x:1000:1000:operator:/home/operator:/bin/shell\n','/etc/group':b'root:x:0:\nwheel:x:10:operator\noperator:x:1000:\n','/etc/shadow':f'root:!:::::::\noperator:{hashed}:::::::\n'.encode()})
        runner.overlay=apply;runner.build=records.build;before=set((runner.ROOT/'.codex-remote-attachments').glob('pkg-boot-admission-*'));saved=sys.argv;sys.argv=[saved[0],'--build-only']
        try:runner.main()
        finally:sys.argv=saved;FILES.clear();FILES.update(saved_files)
        added=set((runner.ROOT/'.codex-remote-attachments').glob('pkg-boot-admission-*'))-before;assert len(added)==1;out=added.pop()
    base=json.loads((out/'manifest.json').read_text());work=out/'workspace';assert not base.get('error') and not base['changed_copies'];assert all(runner.H(work/p)==h for p,h in base['compiled_inputs'].items());assert not (out/'running-root-manifest.json').exists()
    m={'boot_runtime':'INCOMPLETE','preservation':'INCOMPLETE','cases':[],'commands':[],'scope':'actual running root and global account service in a private fixture build'}
    def save():(out/'running-root-manifest.json').write_text(json.dumps(m,indent=2)+'\n')
    def audit(label):
        root=out/label/'root-after.img';raw=root.read_bytes();assert not (struct.unpack_from('<I',raw,1120)[0]&4) and struct.unpack_from('<H',raw,1082)[0]==1,'root not clean'
        for path,expected in (('/home/operator/proof',b'installed-user-write\n'),('/installed-proof',b'enrolled-root-proof\n')):
            dest=out/label/(path.rsplit('/',1)[1]+'.dump');cmd=['debugfs','-R',f'dump {path} {dest}',str(root)];m['commands'].append(cmd);r=subprocess.run(cmd,capture_output=True,text=True);assert not r.returncode and dest.read_bytes()==expected
        for path in ('/run/pkg-proof','/tmp/pkg-proof'):
            r=subprocess.run(['debugfs','-R','stat '+path,str(root)],capture_output=True,text=True);assert 'File not found' in r.stderr
    try:
        configs=[('bios',1)] if args.smoke else [('bios',1),('bios',4),('uefi',1),('uefi',4)]
        def pair(config):
            fw,smp=config;label=f'running-{fw}-{smp}';first=installed_boot(out,work,out/'valid.iso',out/'enrolled.img',fw,smp,label);audit(label)
            second=installed_boot(out,work,out/'valid.iso',out/label/'enrolled.img',fw,smp,label+'-reboot');audit(label+'-reboot');return [first,second]
        with ThreadPoolExecutor(max_workers=len(configs)) as pool:
            for future in as_completed([pool.submit(pair,c) for c in configs]):m['cases'].extend(future.result());save()
        m['boot_runtime']='PASS'
    except BaseException as e:m['boot_runtime']='FAIL';m['error']=repr(e);raise
    finally:
        m['changed_sources']=[p for p,h in base['sources'].items() if runner.H(runner.ROOT/p)!=h];m['changed_guards']=[p for p,h in base['guards'].items() if runner.H(runner.ROOT/p)!=h];m['changed_copies']=[p for p,h in base['compiled_inputs'].items() if runner.H(work/p)!=h];m['preservation']='FAIL' if m['changed_sources'] or m['changed_guards'] or m['changed_copies'] else 'PASS';save();print(out,m['boot_runtime'],m['preservation'],flush=True)
    assert m['preservation']=='PASS'
if __name__=='__main__':main()
