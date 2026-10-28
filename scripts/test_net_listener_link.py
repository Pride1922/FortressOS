#!/usr/bin/env python3
"""Private ISO-only BIOS/UEFI x SMP=1/4 listener replug regression, BSP/AP.

Actual networking, NIC and syscall code; QMP emulated carrier. No physical
PHY/DMA claim. Only the copied snapshot has CPU/owner observation hooks.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import selectors
import shutil
import socket
import subprocess
import tempfile
import time
from test_nmi_transitions import QMP

ROOT=Path(__file__).resolve().parents[1]

def instrument(workspace):
    p=workspace/'src/kernel/syscall.c'; source=p.read_text()
    anchor='    switch (syscall_nr) {'
    assert source.count(anchor)==1
    p.write_text(source.replace(anchor,anchor+'''
        case 0x7ff7: {
            extern int64_t listener_link_fixture_observe(unsigned);
            result=listener_link_fixture_observe((unsigned)frame->rdi); break;
        }'''))
    p=workspace/'src/net/net_control.c'; source=p.read_text()
    anchor='static void step(handoff_token_t t,handoff_request_t r) {'
    assert source.count(anchor)==1
    source=source.replace(anchor,anchor+'''
    if (r.kind==NET_OWNER_TCP_ACCEPT) {
        extern void listener_link_fixture_owner(void); listener_link_fixture_owner();
    }''')
    source+='''
static uint64_t listener_link_fixture_mask;
void listener_link_fixture_owner(void) {
    net_control_assert_owner();
    __atomic_fetch_or(&listener_link_fixture_mask,1ULL<<cpu_current()->id,__ATOMIC_RELEASE);
}
int64_t listener_link_fixture_observe(unsigned operation) {
    return operation ? __atomic_load_n(&listener_link_fixture_mask,__ATOMIC_ACQUIRE) : cpu_current()->id;
}
'''
    p.write_text(source)

def build(workspace,out):
    with (out/'build.log').open('w') as log:
        def run(args,timeout=600):
            subprocess.run(args,cwd=workspace,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=timeout)
        run(['make','-j4','bin/fortress.elf','bin/initramfs.tar'])
        run(['gcc','-O2','-Wall','-Wextra','-Werror','-ffreestanding','-mno-red-zone',
             '-mgeneral-regs-only','-fno-pie','-fno-stack-protector','-Isrc/include',
             '-c','tests/net_listener_link_guest.c','-o','build/listener-link.o'])
        run(['nasm','-f','elf64','-DTOOL_ENTRY=listener_link_main','user/tools/start.asm',
             '-o','build/listener-link-start.o'])
        run(['ld','-m','elf_x86_64','-nostdlib','-static','-z','noexecstack','-T','user/shell.ld',
             'build/listener-link-start.o','build/listener-link.o','-o','build/initramfs/bin/link-probe'])
        run(['python3','scripts/create_initramfs.py','build/initramfs','bin/initramfs.tar'])
        run(['make','bin/fortress.iso'])

def boot(workspace,out,firmware,cpus):
    label=f'{firmware}-smp{cpus}'
    with tempfile.TemporaryDirectory(prefix='listener-link-qmp-') as temporary:
        tmp=Path(temporary)
        with socket.socket() as reserve:
            reserve.bind(('127.0.0.1',0)); port=reserve.getsockname()[1]
        argv=['qemu-system-x86_64','-M','q35','-m','512M','-smp',str(cpus),'-accel','tcg',
              '-cpu','max,+rdrand,+rdseed','-display','none','-monitor','none','-serial','stdio',
              '-no-reboot','-boot','d','-cdrom',str(workspace/'bin/fortress.iso'),
              '-netdev',f'user,id=n0,hostfwd=tcp:127.0.0.1:{port}-:9000',
              '-device','e1000e,netdev=n0,id=nic0','-qmp',f'unix:{tmp/"qmp.sock"},server=on,wait=off']
        if firmware=='uefi':
            shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',tmp/'vars.fd')
            argv+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
                   '-drive',f'if=pflash,format=raw,unit=1,file={tmp/"vars.fd"}']
        # Audit the final argv: ISO and optional private pflash only; no data disks.
        assert argv.count('-cdrom')==1 and '-hda' not in argv and '-blockdev' not in argv
        assert all(argv[i+1].startswith('if=pflash,') for i,a in enumerate(argv) if a=='-drive')
        assert all(not any(x in a for x in ('nvme,','usb-storage,','virtio-blk','scsi-hd')) for a in argv)
        (out/(label+'.argv.json')).write_text(json.dumps(argv,indent=2)+'\n')
        data=bytearray(); events=[]
        err=(out/(label+'.stderr.log')).open('wb')
        proc=subprocess.Popen(argv,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err)
        sel=selectors.DefaultSelector(); sel.register(proc.stdout,selectors.EVENT_READ)
        def text(start=0):
            return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',data[start:].decode(errors='replace')).replace('\r','')
        def wait(predicate,start=0,timeout=45):
            end=time.monotonic()+timeout
            while time.monotonic()<end:
                for key,_ in sel.select(.05): data.extend(key.fileobj.read1(65536))
                current=text(start)
                assert not any(s in current for s in ('LISTENER LINK FAIL','KERNEL PANIC','[FATAL]','[PROCESS FAULT]')),current[-3000:]
                if predicate(current): return current
                assert proc.poll() is None,current[-3000:]
            raise TimeoutError(text(start)[-3000:])
        def send(cmd): proc.stdin.write((cmd+'\n').encode()); proc.stdin.flush()
        def command(cmd):
            start=len(data);send(cmd)
            return wait(lambda t:cmd[-40:]+'\n' in t and t.endswith('$ '),start)
        qmp=None
        try:
            wait(lambda t:t.endswith('FortressOS login: '),timeout=90)
            send('operator');wait(lambda t:t.endswith('$ '),timeout=20)
            qmp=QMP(tmp/'qmp.sock')
            for cpu in range(cpus):
                start=len(data);send(f'/bin/link-probe --worker {cpu}')
                wait(lambda t:'LISTENER LINK READY\n' in t,start)
                qmp.execute('set_link',{'name':'nic0','up':False});events.append({'cpu':cpu,'up':False})
                wait(lambda t:'LISTENER LINK DOWN\n' in t,start)
                qmp.execute('set_link',{'name':'nic0','up':True});events.append({'cpu':cpu,'up':True})
                wait(lambda t:'LISTENER LINK FRESH\n' in t,start)
                with socket.create_connection(('127.0.0.1',port),timeout=10) as peer:
                    peer.settimeout(10); payload=b'REPLUG-FRESH\n';peer.sendall(payload); received=b''
                    while len(received)<len(payload):
                        chunk=peer.recv(len(payload)-len(received));assert chunk,'early EOF';received+=chunk
                    assert received==payload,received
                wait(lambda t:'LISTENER LINK PASS\n' in t and t.endswith('$ '),start)
                status=command('echo $?');assert re.search(r'\n0\n',status),status
            send('sudo /bin/poweroff');wait(lambda t:'[POWER] Shutdown initiated' in t)
            assert proc.wait(timeout=20)==0
            return {'case':label,'result':'PASS','application_cpus':list(range(cpus)),
                    'owner_mask':1,'fresh_echo':'REPLUG-FRESH\n','carrier_events':events}
        finally:
            (out/(label+'.serial.log')).write_bytes(data)
            (out/(label+'.qmp.json')).write_text(json.dumps(events,indent=2)+'\n')
            if proc.poll() is None: proc.kill();proc.wait(timeout=5)
            if qmp: qmp.stream.close();qmp.sock.close()
            sel.close();err.close()

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--cpus',type=int,nargs='+',default=[1,4],choices=[1,4,8])
    parser.add_argument('--firmware',nargs='+',default=['bios','uefi'],choices=['bios','uefi'])
    parser.add_argument('--workspace',type=Path,help='Resume an already instrumented private snapshot')
    args=parser.parse_args()
    parent=ROOT/'build/listener-link-loss';parent.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='qemu-',dir=parent));print(out,flush=True)
    result={'result':'FAIL','cases':[]}
    try:
        workspace=args.workspace.resolve() if args.workspace else Path(subprocess.check_output(
            ['python3','scripts/create_ext4_guest_workspace.py'],cwd=ROOT,text=True).strip())
        assert workspace.is_relative_to(ROOT/'.codex-remote-attachments')
        result['workspace']=str(workspace)
        if not args.workspace: instrument(workspace)
        else: assert 'listener_link_fixture_mask' in (workspace/'src/net/net_control.c').read_text()
        build(workspace,out)
        result['hashes']={n:hashlib.sha256((workspace/'bin'/n).read_bytes()).hexdigest()
                          for n in ('fortress.elf','initramfs.tar','fortress.iso')}
        for firmware in args.firmware:
            for cpus in args.cpus:
                print(f'Testing {firmware} SMP={cpus}',flush=True)
                case=boot(workspace,out,firmware,cpus);result['cases'].append(case);print(case,flush=True)
        result['result']='PASS'
    except BaseException as error:
        result['failure']=repr(error);raise
    finally: (out/'manifest.json').write_text(json.dumps(result,indent=2)+'\n')

if __name__=='__main__': main()
