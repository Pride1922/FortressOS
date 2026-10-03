#!/usr/bin/env python3
"""Cold/warm link recovery: BIOS/UEFI x e1000/e1000e, disposable ISO,
no data disks. QMP toggles emulated carrier; not an I219 PHY/DMA claim.
Retains argv, UART, pcap, QMP events and failures before reaping QEMU.
"""
import argparse
import concurrent.futures
import hashlib
import json
import shutil
import socket
import struct
import subprocess
import time
import traceback
import uuid
from pathlib import Path
from test_net_eth import build_iso
from test_net_tcp_matrix import Case, save
from test_net_pci import REPO, CODE, VARS
from net_tcp_wire_audit import records
from test_nmi_transitions import Remote


class LinkCase(Case):
    def qmp(self, command):
        end=time.monotonic()+5
        while True:
            monitor=socket.socket(socket.AF_UNIX); monitor.settimeout(5)
            try: monitor.connect(str(self.qmp_path)); break
            except (FileNotFoundError,ConnectionRefusedError):
                monitor.close()
                assert self.proc.poll() is None,'QEMU exited before QMP connection'
                assert time.monotonic()<end,'QMP creation timeout'
                time.sleep(.01)
        with monitor:
            with monitor.makefile('rb') as reader:
                assert 'QMP' in json.loads(reader.readline())
                def query(value):
                    monitor.sendall(json.dumps(value).encode()+b'\n')
                    while True:
                        result=json.loads(reader.readline())
                        if 'error' in result: raise AssertionError(result)
                        if 'return' in result: return result
                query({'execute':'qmp_capabilities'})
                result=query(command)
        self.events.append(dict(command=command,result=result))
        save(self.root/'qmp-events.json',self.events)

    def link(self, up):
        self.qmp({'execute':'set_link','arguments':{'name':'nic0','up':up}})

    def boot_link(self, cold):
        self.events=[]
        iso=build_iso(self.root,'')
        versions=subprocess.check_output(['qemu-system-x86_64','--version'],text=True)
        versions+='git '+subprocess.check_output(['git','rev-parse','HEAD'],cwd=REPO,text=True)
        sources=self.root/'fixture-sources'; sources.mkdir()
        for name in ('test_net_link.py','test_net_tcp_matrix.py','test_net_eth.py','test_nmi_transitions.py','net_tcp_wire_audit.py'):
            raw=(REPO/'scripts'/name).read_bytes(); (sources/name).write_bytes(raw)
            versions+=name+' sha256 '+hashlib.sha256(raw).hexdigest()+'\n'
        for name in ('fortress.elf','initramfs.tar'):
            raw=(self.root/'root/boot'/name).read_bytes()
            versions+=name+' sha256 '+hashlib.sha256(raw).hexdigest()+'\n'
        (self.root/'versions.txt').write_text(versions)
        path=Path('/tmp')/('fortress-link-'+uuid.uuid4().hex[:12])
        self.uart_path=path; self.qmp_path=Path(str(path)+'-qmp')
        self.gdb_path=Path(str(path)+'-gdb')
        self.host=socket.socket(); self.host.bind(('127.0.0.1',0))
        self.forward=self.host.getsockname()[1]; self.host.close(); self.host=None
        cmd=['qemu-system-x86_64','-M','q35','-m','2G','-accel','tcg','-smp','1',
             '-display','none','-monitor','none','-no-reboot','-S','-boot','d','-cdrom',str(iso),
             '-chardev',f'socket,id=uart,path={path},server=on,wait=on','-serial','chardev:uart',
             '-qmp',f'unix:{self.qmp_path},server=on,wait=off',
             '-gdb',f'unix:{self.gdb_path},server=on,wait=off',
             '-netdev',f'user,id=net0,hostfwd=tcp:127.0.0.1:{self.forward}-10.0.2.15:9000',
             '-device',f'{self.model},id=nic0,netdev=net0,mac=52:54:00:12:34:56',
             '-object',f'filter-dump,id=dump0,netdev=net0,file={self.pcap}']
        if self.mode=='uefi':
            variables=self.root/'vars.fd'; shutil.copyfile(VARS,variables)
            cmd+=['-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
                  '-drive',f'if=pflash,format=raw,unit=1,file={variables}']
        frozen=tuple(cmd)
        def preflight(candidate): assert tuple(candidate)==frozen,'unauthorized argv/storage'
        preflight(cmd)
        for extra in (['-drive','file=unsafe.img'],['-device','nvme'],['-blockdev','driver=file,filename=unsafe.img']):
            try: preflight(cmd+extra)
            except AssertionError: pass
            else: raise AssertionError('unsafe argv accepted')
        save(self.root/'argv.json',cmd)
        save(self.root/'scenario.json',dict(mode=self.mode,model=self.model,cold=cold,data_disks=0))
        stderr=(self.root/'qemu-stderr.log').open('wb'); self.files.append(stderr)
        self.proc=subprocess.Popen(cmd,cwd=REPO,stdout=subprocess.DEVNULL,stderr=stderr)
        end=time.monotonic()+20
        while time.monotonic()<end:
            try:
                self.uart=socket.socket(socket.AF_UNIX); self.uart.connect(str(path)); break
            except OSError:
                self.uart.close(); self.uart=None; time.sleep(.03)
        assert self.uart is not None,'UART creation timeout'
        self.uart.settimeout(.05); self.start_reader()
        if cold:
            # e1000e firmware/device reset can undo QMP's initial carrier bit.
            # Stop at the unmodified driver's entry, then set carrier down.
            # No guest memory/register writes or test-only driver branch.
            symbols={fields[2]:int(fields[0],16) for line in subprocess.check_output(
                ['nm','-an',str(REPO/'bin/fortress.elf')],text=True).splitlines()
                if len(fields:=line.split())==3}
            remote=Remote(self.gdb_path)
            try:
                remote.resume_to(symbols['e1000_boot_probe'])
                save(self.root/'cold-boundary.json',dict(symbol='e1000_boot_probe',
                    address=hex(symbols['e1000_boot_probe']),pc=hex(remote.registers()[0][16])))
                self.link(False)
                assert remote.request('D')=='OK'
            finally: remote.sock.close()
        self.qmp({'execute':'cont'})
        # QEMU 8.2 e1000e_autoneg_resume clears link_down on VM resume.
        # Reassert the external carrier immediately while running, before its
        # 500-ms negotiation timer fires. No guest state is patched.
        if cold: self.link(False)
        self.wait(lambda t:'fortress> ' in t and 'BSP ingress worker started' in t)
        if cold:
            assert 'Waiting for cable; DMA not allocated' in self.text()
            assert 'DMA ready' not in self.text()

    def ping(self, success):
        result=self.finish(self.start('ping -c 1 10.0.2.2'),15)
        assert ('1 probes, 1 replies' if success else 'transmit failed') in result,result

    def listener(self):
        at=self.start('nc -l 9000')
        # QEMU connection forwarding may race guest LISTEN publication.
        end=time.monotonic()+10
        while True:
            try: peer=socket.create_connection(('127.0.0.1',self.forward),timeout=1); break
            except OSError:
                assert time.monotonic()<end; self.pump()
        with peer:
            peer.settimeout(10); peer.sendall(b'link-recovered\n'); peer.shutdown(socket.SHUT_WR)
            while peer.recv(1024): pass
        result=self.finish(at,15)
        assert '\nlink-recovered\n' in result and 'nc: socket or I/O failure' not in result,result


def run(root,mode,model,cold):
    case=LinkCase(root,mode,model,'user')
    try:
        case.boot_link(cold)
        if cold:
            case.ping(False)
            result=case.finish(case.start('nslookup -s 10.0.2.2 service.test'),15)
            assert 'nslookup: DNS_IO' in result,result
            case.link(True); case.wait(lambda t:'DMA ready' in t,15)
        case.ping(True)
        at=case.start('ping -c 4 10.0.2.2')
        case.wait(lambda t:'32 bytes from 10.0.2.2' in t[at:],15)
        case.link(False)
        result=case.finish(at,15)
        assert 'transmit failed' in result and '4 probes' in result,result
        up=len(case.text()); case.link(True)
        case.wait(lambda t:'Online; retained rings' in t[up:],15)
        case.ping(True)
        # Unplug while ACCEPT is blocked: old command fails, new one works.
        at=case.start('nc -l 9000')
        until=time.monotonic()+1
        while time.monotonic()<until: case.pump()
        assert '[PROCESS] Exit' not in case.text()[at:],'listener exited before link loss'
        case.link(False)
        result=case.finish(at,15)
        # Current finite nc reports I/O failure through its exit status, without
        # the old diagnostic string. Still require blocked-before-loss, bounded
        # failure and a fresh listener's independently checked recovery below.
        assert '[PROCESS] Exit status 1' in result,result
        status=case.finish(case.start('echo $?'),15)
        assert '1' in status.splitlines(),status
        case.ping(False)
        case.link(True); case.wait(lambda t:'Online; retained rings' in t,15)
        case.ping(True); case.listener(); case.ping(True)
        for _ in range(3):
            down=len(case.text()); case.link(False)
            case.wait(lambda t:'Cable disconnected; retained rings' in t[down:],15)
            up=len(case.text()); case.link(True)
            case.wait(lambda t:'Online; retained rings' in t[up:],15)
            case.ping(True)
        assert case.text().count('DMA ready')==1,case.text()
        assert 'PANIC' not in case.text() and '[FATAL]' not in case.text()
    except BaseException:
        case.failure=traceback.format_exc(); case.diagnostics()
    finally:
        try: case.close()
        finally:
            if hasattr(case,'gdb_path'): case.gdb_path.unlink(missing_ok=True)
    try:
        audit_echo(case.pcap,root)
    except BaseException:
        manifest=json.loads((root/'result.json').read_text())
        manifest['status']='FAIL'; manifest['icmp_audit_error']=traceback.format_exc()
        save(root/'result.json',manifest); raise
    print('[PASS]',root.name,flush=True)


def audit_echo(pcap,root):
    # Independent capture sanity: actual outbound echo and inbound reply.
    directions=set()
    for _,raw in records(pcap):
        if len(raw)<42 or raw[12:14]!=b'\x08\x00' or raw[23]!=1: continue
        h=(raw[14]&15)*4
        total=struct.unpack_from('!H',raw,16)[0]
        assert total>=h+8 and 14+total<=len(raw)
        body=raw[14+h:14+total]
        padded=body+b'\0'*(len(body)%2)
        checksum=sum(struct.unpack('!'+str(len(padded)//2)+'H',padded))
        while checksum>>16: checksum=(checksum&65535)+(checksum>>16)
        assert checksum==65535,'invalid ICMP checksum'
        directions.add(body[0])
    assert {0,8}<=directions,'capture missing successful echo directions'
    save(root/'icmp-audit.json',dict(types=sorted(directions),checksum='PASS'))


def main():
    parser=argparse.ArgumentParser(); parser.add_argument('--jobs',type=int,choices=(1,2,4),default=2)
    args=parser.parse_args()
    root=REPO/'build/net-link'/uuid.uuid4().hex[:8]
    print('Link artifacts:',root,flush=True)
    cases=[(m,n,c) for m in ('bios','uefi') for n in ('e1000','e1000e') for c in (True,False)]
    failures=[]
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.jobs) as pool:
        futures=[pool.submit(run,root/f"{m}-{n}-{'cold' if c else 'warm'}",m,n,c) for m,n,c in cases]
        for future in concurrent.futures.as_completed(futures):
            try: future.result()
            except Exception: failures.append(traceback.format_exc())
    save(root/'matrix.json',dict(total=8,passed=8-len(failures),failures=failures))
    assert not failures,'\n'.join(failures)
    print('NET link recovery 8/8 PASS (emulated carrier; physical gates pending)',flush=True)


if __name__=='__main__': main()
