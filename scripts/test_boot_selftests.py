#!/usr/bin/env python3
"""Normal/diagnostic boot matrix. Private ISOs; no storage devices attached."""
import argparse,json,os,re,selectors,shutil,subprocess,tempfile,time
from pathlib import Path
from test_net_eth import build_iso,REPO

ABSENT=('Physical Memory Manager self-tests passed','[CAUGHT] Expected #PF',
        'Dynamic Kernel Heap verification suite','Cooperative Multitasking Suite',
        'Preemptive Scheduler Suite','Lifecycle Stress & Heap Sync Suite',
        'Phase 7 (Checkpoint 0): Address-Space','SMP Piece 3: Lock Discipline',
        'SMP Piece 4: The SMP Scheduler','Phase 8 (Step 8A): Basic Framebuffer')

def run(out,iso,firmware,cpus,diagnostic,accel):
    case=out/f'{firmware}-{cpus}-{"diagnostic" if diagnostic else "normal"}'
    case.mkdir();argv=['qemu-system-x86_64','-M','q35','-m','2G','-smp',str(cpus),'-accel',accel,
        '-cpu','host' if accel=='kvm' else 'max','-display','none','-monitor','none','-no-reboot',
        '-serial','stdio','-net','none','-boot','d','-cdrom',str(iso)]
    if firmware=='uefi':
        variables=case/'vars.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
        argv+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
               '-drive',f'if=pflash,format=raw,unit=1,file={variables}']
    assert not any('/dev/' in x for x in argv)
    (case/'argv.json').write_text(json.dumps(argv,indent=2)+'\n')
    data=bytearray();error=(case/'stderr.log').open('wb')
    proc=subprocess.Popen(argv,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=error)
    selector=selectors.DefaultSelector();selector.register(proc.stdout,selectors.EVENT_READ)
    def text():return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',data.decode(errors='replace')).replace('\r','')
    def wait(predicate,timeout=120):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            for key,_ in selector.select(.1):
                chunk=os.read(key.fileobj.fileno(),65536)
                if chunk:data.extend(chunk)
            t=text();assert '[FAIL]' not in t and '[PANIC]' not in t,t[-6000:]
            if predicate(t):return t
            assert proc.poll() is None,t[-6000:]
        raise TimeoutError(text()[-6000:])
    def send(s):proc.stdin.write((s+'\n').encode());proc.stdin.flush()
    try:
        boot=wait(lambda t:'FortressOS login: ' in t)
        for message in ('PMM audit passed','High-memory allocation unlocked','ACPI, interrupt controllers and timer ready','CPU discovery complete'):
            assert message in boot,message
        if diagnostic:
            for message in ABSENT:assert message in boot,message
        else:
            for message in ABSENT:assert message not in boot,message
            assert '[TRAP] Exception' not in boot and '[FAULT-INJECT]' not in boot
        send('operator');wait(lambda t:re.search(r'operator@[^\n]+\$ $',t))
        send('echo BOOT_MODE_READY');wait(lambda t:re.search(r'(?m)^BOOT_MODE_READY$',t))
        send('sudo /sbin/shutdown');wait(lambda t:'[POWER] Shutdown initiated' in t)
        assert proc.wait(timeout=45)==0
        record={'firmware':firmware,'cpus':cpus,'diagnostic':diagnostic,'result':'PASS',
                'login_and_userspace':'PASS','shutdown':'PASS','storage_devices':0}
        print(record,flush=True);return record
    finally:
        if proc.poll() is None:proc.kill();proc.wait(timeout=10)
        selector.close();error.close();(case/'serial.log').write_bytes(data)

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--accel',choices=('kvm','tcg'),default='kvm');args=parser.parse_args()
    out=Path(tempfile.mkdtemp(prefix='boot-selftests-',dir=REPO/'build'));print('Evidence:',out,flush=True)
    report={'result':'INCOMPLETE','physical_writes':False,'cases':[]}
    try:
        isos={}
        for diagnostic in (False,True):
            media=out/('diagnostic' if diagnostic else 'normal');media.mkdir()
            cmdline='boot_mode=live console=text'+(' boot_selftests=1' if diagnostic else '')
            isos[diagnostic]=build_iso(media,cmdline)
        for firmware in ('bios','uefi'):
            for cpus in (1,4):
                for diagnostic in (False,True):report['cases'].append(run(out,isos[diagnostic],firmware,cpus,diagnostic,args.accel))
        report['result']='PASS'
    finally:
        for iso in out.rglob('*.iso'):iso.unlink()
        (out/'manifest.json').write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
