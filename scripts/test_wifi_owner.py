#!/usr/bin/env python3
"""Isolated ISO-only BIOS/UEFI x SMP=1/4/8, actual AP Ring 3 SYS_WIFI.

Radio hooks and an inert pending gate exist only in the copied test workspace.
No data disks, production affinity changes or real Intel acceptance claims.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import test_poll_nc as guest_api

ROOT = Path(__file__).resolve().parents[1]

def patch(workspace, name, anchor, replacement):
    p = workspace / name
    source = p.read_text()
    assert source.count(anchor) == 1, (name, anchor)
    p.write_text(source.replace(anchor, replacement))

def instrument(workspace):
    shutil.copyfile(workspace/'tests/wifi_owner_fixture.c', workspace/'src/net/wifi_owner_fixture.c')
    patch(workspace, 'src/net/net_control.c', '    owner_tid=thread_current()->tid;',
          '    owner_tid=thread_current()->tid;\n    extern void wifi_fixture_init(void);\n    wifi_fixture_init();')
    patch(workspace, 'src/net/net_wifi_owner.c',
          'void net_wifi_step(handoff_token_t t,handoff_request_t r) {',
          '''void net_wifi_step(handoff_token_t t,handoff_request_t r) {
    extern bool wifi_fixture_gate(unsigned);
    if (wifi_fixture_gate(r.kind) && !handoff_cancelled(t)) {
        handoff_finish(t,(handoff_reply_t){0},true); return;
    }''')
    patch(workspace, 'src/drivers/iwlwifi.c',
          'bool iwlwifi_load_firmware(const void *fw_data, size_t fw_len) {',
          'bool wifi_fixture_real_load(const void *fw_data, size_t fw_len) {')
    patch(workspace, 'src/kernel/syscall.c', '    uint64_t syscall_nr = frame->rax;',
          '''    if ((frame->cs & 3)==3 && frame->rax==SYS_WIFI) {
        extern void wifi_fixture_entry(void); wifi_fixture_entry();
    }
    uint64_t syscall_nr = frame->rax;''')
    patch(workspace, 'src/kernel/syscall.c', '    switch (syscall_nr) {',
          '''    switch (syscall_nr) {
        case 0x7ff6: {
            extern int64_t wifi_fixture_observe(unsigned);
            result=wifi_fixture_observe((unsigned)frame->rdi); break;
        }''')

def boot(workspace, out, firmware, cpus):
    label=f'{firmware}-smp{cpus}'
    guest_api.ROOT=workspace
    guest=guest_api.Guest(firmware,cpus,out)
    def raw(cmd, marker=None):
        start=len(guest.data); guest.send(cmd)
        return guest.wait(lambda s:cmd+'\n' in s and s.endswith('$ ') and
                          (not marker or marker in s),start,timeout=45)
    def command(cmd, marker=None):
        text=raw(cmd,marker)
        if marker: assert marker in text, text
        check=raw('echo $?')
        assert re.search(r'(?:^|\n)0\n',check), (cmd,text,check)
        return text
    def probe(cpu, mode): return f'/bin/wifi-owner-probe --worker {cpu} {mode}'
    try:
        guest.wait(lambda t:t.endswith('FortressOS login: '),timeout=90)
        guest.send('operator'); guest.wait(lambda t:t.endswith('$ '),timeout=20)
        for cpu in range(cpus): command(probe(cpu,'a'),'WIFI BSP/AP operations PASS')
        # Pending caller on BSP/AP, interrupt/stop/death. Owner still serves
        # unrelated NETCTL queries; retained cleanup completes after interest ends.
        for signal in ('STOP','INT','KILL'):
            command(probe(0,'h'))
            launched=raw(probe(cpus-1,'c' if signal=='KILL' else 'w')+' &')
            job=int(re.findall(r'\[(\d+)\]\s+\d+',launched)[-1])
            command(probe(0,'p'))
            command(f'kill %{job} {signal}')
            if signal=='STOP':
                command(probe(0,'u'))
                command(f'kill %{job} CONT')
            else: command(probe(0,'u'))
        command(probe(0,'g'),'WIFI ownership/cleanup PASS')
        command(probe(0,'h'))
        for i in range(4): raw(probe(i%cpus,'w')+' &')
        command(probe(0,'f'))
        command(probe(0,'e'))
        command(probe(0,'u'))
        command(probe(0,'g'),'WIFI ownership/cleanup PASS')
        for cpu in range(cpus):
            launched=raw(probe(cpu,'b')+' &')
            assert re.search(r'\[\d+\]\s+\d+',launched),launched
        # Reaping/retained cleanup is bounded independently of caller scheduling.
        guest.wait(lambda t:t.count('WIFI concurrent PASS')>=cpus,timeout=45)
        command(probe(0,'g'),'WIFI ownership/cleanup PASS')
        command('/bin/ping 10.0.2.2')
        command('/bin/nslookup example.com')
        log=guest.text()
        assert not any(s in log for s in ('WIFI OWNER FAIL','KERNEL PANIC','[FATAL]','[PROCESS FAULT]')),log[-4000:]
        return {'case':label,'result':'PASS','application_cpus':list(range(cpus)),
                'owner_cpu':0,'radio':'mock; no Intel RF/DMA evidence'}
    finally:
        (out/(label+'.serial.log')).write_bytes(guest.data)
        guest.close()

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--cpus',nargs='+',type=int,default=[1,4,8])
    parser.add_argument('--firmware',nargs='+',default=['bios','uefi'])
    parser.add_argument('--workspace',type=Path,help='Resume an already instrumented private snapshot')
    args=parser.parse_args()
    assert set(args.cpus)<={1,4,8} and set(args.firmware)<={'bios','uefi'}
    parent=ROOT/'build/wifi-owner'; parent.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='qemu-',dir=parent)); print(out,flush=True)
    result={'result':'FAIL','cases':[]}
    try:
        workspace=args.workspace.resolve() if args.workspace else Path(subprocess.check_output(['python3','scripts/create_ext4_guest_workspace.py'],cwd=ROOT,text=True).strip())
        result['workspace']=str(workspace)
        assert workspace.is_relative_to(ROOT/'.codex-remote-attachments')
        if not args.workspace: instrument(workspace)
        else: assert (workspace/'src/net/wifi_owner_fixture.c').is_file()
        with (out/'build.log').open('w') as log:
            subprocess.run(['make','-j4','bin/fortress.elf','bin/initramfs.tar'],cwd=workspace,
                           stdout=log,stderr=subprocess.STDOUT,check=True,timeout=600)
            subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-ffreestanding',
                '-mno-red-zone','-mgeneral-regs-only','-fno-pie','-fno-stack-protector',
                '-Isrc/include','-c','tests/wifi_owner_guest.c','-o','build/wifi-probe.o'],
                cwd=workspace,stdout=log,stderr=subprocess.STDOUT,check=True)
            subprocess.run(['nasm','-f','elf64','-DTOOL_ENTRY=wifi_owner_main','user/tools/start.asm',
                            '-o','build/wifi-start.o'],cwd=workspace,stdout=log,stderr=subprocess.STDOUT,check=True)
            subprocess.run(['ld','-m','elf_x86_64','-nostdlib','-static','-z','noexecstack','-T','user/shell.ld',
                'build/wifi-start.o','build/wifi-probe.o','-o','build/initramfs/bin/wifi-owner-probe'],
                cwd=workspace,stdout=log,stderr=subprocess.STDOUT,check=True)
            subprocess.run(['python3','scripts/create_initramfs.py','build/initramfs','bin/initramfs.tar'],
                           cwd=workspace,stdout=log,stderr=subprocess.STDOUT,check=True)
            subprocess.run(['make','bin/fortress.iso'],cwd=workspace,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
        result['hashes']={n:hashlib.sha256((workspace/'bin'/n).read_bytes()).hexdigest()
                          for n in ('fortress.elf','initramfs.tar','fortress.iso')}
        for firmware in args.firmware:
            for cpus in args.cpus:
                print(f'Testing {firmware} SMP={cpus}',flush=True)
                case=boot(workspace,out,firmware,cpus); result['cases'].append(case)
                (out/'manifest.json').write_text(json.dumps(result,indent=2)+'\n')
                print(case,flush=True)
        result['result']='PASS'
    except Exception as error:
        result['failure']=repr(error)
        raise
    finally:
        (out/'manifest.json').write_text(json.dumps(result,indent=2)+'\n')

if __name__=='__main__': main()
