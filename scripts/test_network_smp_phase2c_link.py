#!/usr/bin/env python3
# Installed-root carrier-loss gate. Retain the effective driver and probe override.
import inspect
import json
import socket
import tempfile
import time
from pathlib import Path
import test_network_smp_phase2c as base
real_audit=base.audit
real_run=base.baseline.fixture.run
last_out=None
qmp_root=Path(tempfile.mkdtemp(prefix='smp2c-qmp-'))

def link_audit(argv,out,label,nic='e1000'):
    global last_out
    assert real_audit(argv,out,label,nic)
    last_out=out; index=argv.index(f'{nic},netdev=net0')
    argv[index]+=',id=nic0'
    # DrvFS does not support AF_UNIX bind; only this transient control socket
    # lives in a native Linux fixture directory. All evidence stays under out.
    endpoint=qmp_root/(label+'.sock')
    assert endpoint.resolve().parent==qmp_root.resolve() and len(str(endpoint).encode())<104
    argv+=['-qmp',f'unix:{endpoint},server=on,wait=off']
    check=argv[:-2].copy(); check[index]=f'{nic},netdev=net0'
    assert real_audit(check,out,label,nic)
    (out/'effective-link-boot.py').write_text(effective_boot)
    (out/'qmp-fixture-directory.json').write_text(json.dumps({'native_socket_directory':str(qmp_root),'scope':'transient QMP control sockets only'},indent=2))
    return True

def link_set(out,label,up):
    exchanges=[]
    with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as peer:
        peer.settimeout(5); peer.connect(str(qmp_root/(label+'.sock')))
        with peer.makefile('rwb') as stream:
            greeting=json.loads(stream.readline()); assert 'QMP' in greeting
            exchanges.append(greeting)
            for identity,command in enumerate(({'execute':'qmp_capabilities'},
                    {'execute':'set_link','arguments':{'name':'nic0','up':up}}),1):
                command['id']=identity
                stream.write((json.dumps(command)+'\n').encode()); stream.flush(); exchanges.append(command)
                while True:
                    line=stream.readline(); assert line
                    response=json.loads(line); exchanges.append(response)
                    if response.get('id')==identity:
                        assert 'return' in response,response
                        break
    log=out/(label+'.qmp.json'); saved=json.loads(log.read_text()) if log.is_file() else []
    # QEMU pcap timestamps use a different clock epoch on this host. Establish
    # ordering by the complete capture prefix before any fresh client starts.
    deadline=time.monotonic()+2
    while True:
        try: captured=sum(1 for _ in base.records(out/(label+'.pcap'))); break
        except AssertionError:
            assert time.monotonic()<deadline
            time.sleep(.01)
    saved.append({'up':up,'completed_ns':time.time_ns(),'capture_records_at_completion':captured,'exchanges':exchanges})
    log.write_text(json.dumps(saved,indent=2))

def probe_run(argv,log,timeout=120):
    global last_out
    original=next((Path(str(a)) for a in argv if str(a).endswith('/tests/net_tcp_owner_guest_user.c')),None)
    if original:
        out=Path(log).parent; last_out=out
        text=original.read_text().replace('#include "../user/','#include "'+str(original.parent.parent/'user')+'/')
        text=text.replace('return info.uptime_ticks+seconds*info.tick_hz;','(void)seconds; return info.uptime_ticks+50*info.tick_hz;')
        text=text.replace('result==SYSCALL_ETIMEDOUT || result==SYSCALL_EINTR','result==SYSCALL_EIO')
        text=text.replace('result==SYSCALL_ETIMEDOUT ? 0 : 51','result==SYSCALL_EIO ? 0 : 51')
        text=text.replace('status==SYSCALL_ETIMEDOUT ? 0 : 36','status==SYSCALL_EIO ? 0 : 36')
        variant=out/'link-probe.c'; variant.write_text(text)
        argv=[str(variant) if Path(str(a))==original else a for a in argv]
        (out/'link-probe-override.json').write_text(json.dumps({'original':str(original),
            'original_sha256':base.baseline.fixture.digest(original),'compiled':str(variant),
            'compiled_sha256':base.baseline.fixture.digest(variant),'build_argv':argv},indent=2))
    return real_run(argv,log,timeout)

def link_wire_audit(path,unused):
    flows={}; bases={}; zero=False
    for ordinal,(stamp,raw) in enumerate(base.records(path)):
        p=base.audited_packet(raw)
        if not p: continue
        if p['source']==17779 and not p['window']: zero=True
        if 17778 not in (p['source'],p['dest']): continue
        port=p['source'] if p['dest']==17778 else p['dest']; direction=0 if p['dest']==17778 else 1
        item=flows.setdefault(port,{'streams':({},{}),'syn':0})
        if p['flags']&2:
            bases[port,direction]=(p['seq']+1)&0xffffffff
            if not direction: item['syn']=ordinal
        for i,value in enumerate(p['data']):
            offset=(p['seq']+i-bases[port,direction])&0xffffffff; stream=item['streams'][direction]
            assert offset not in stream or stream[offset]==value
            stream[offset]=value
    events=json.loads(path.with_suffix('.qmp.json').read_text()); assert [e['up'] for e in events]==[False,True]
    up=events[-1]['capture_records_at_completion']; exact=0
    for item in flows.values():
        sent,received=(bytes(s[k] for k in sorted(s)) for s in item['streams'])
        if not sent and not received: continue
        assert item['syn']>=up and sent==received==bytes((i*37+(i>>8))&255 for i in range(16384))
        exact+=1
    assert zero and exact==1,(zero,exact)
    path.with_suffix('.link-wire-audit.json').write_text(json.dumps({'result':'PASS','zero_window_before_loss':True,'fresh_post_recovery_bytes':16384},indent=2))

effective_boot=inspect.getsource(base.boot)
a=effective_boot.index("        command('net-cpu-probe h')"); b=effective_boot.index("        config='address",a)
effective_boot=effective_boot[:a]+'''        for cpu,mode in ((3,'c'),(1,'w'),(2,'z')):
            command(f'net-cpu-probe --worker {cpu if cpus>1 else 0} {mode} &','')
        command(probe('o'))
        command(probe('m'))
        command('/sbin/ifconfig','link UP')
        link_set(out,label,False)
        until=time.monotonic()+30
        while True:
            observed=command('/sbin/ifconfig','')
            if 'link DOWN' in observed: break
            assert time.monotonic()<until,observed
        until=time.monotonic()+30
        while True:
            observed=command('jobs','')
            if not re.search(r'\\b(?:Running|Stopped)\\b',observed): break
            assert time.monotonic()<until,observed
        assert not re.findall(r'Done\\([1-9][0-9]*\\)',text()),text()[-4000:]
        link_set(out,label,True)
        until=time.monotonic()+30
        while True:
            observed=command('/sbin/ifconfig','')
            if 'link UP' in observed: break
            assert time.monotonic()<until,observed
        command('ping -c 1 10.0.2.2','bytes from 10.0.2.2')
        command(probe('g'))
        command(probe('t'))
        command(probe('g'))
'''+effective_boot[b:]
a=effective_boot.index("    entries=re.findall("); b=effective_boot.index("    root=out/",a)
effective_boot=effective_boot[:a]+'''    log=text()
    assert '[NET SMP2C CLEAN]' in log
    assert not re.findall(r'Done\\([1-9][0-9]*\\)',log),log[-4000:]
    entries=re.findall(r'\\[NET SMP2C MASK\\] nr=(0x[0-9A-Fa-f]+) cpus=(0x[0-9A-Fa-f]+)',log)
    masks={int(nr,16):int(mask,16) for nr,mask in entries}
    if cpus>1:
        assert masks.get(38,0)&14==14 and masks.get(39,0)&14==14,masks
        assert masks.get(43,0)&6==6 and masks.get(51,0)&10==10,masks
        assert masks.get(49,0)&6==6 and masks.get(50,0)&2==2,masks
    owner_masks=re.findall(r'\\[NET SMP2C OWNER\\] cpus=(0x[0-9A-Fa-f]+) steps=(0x[0-9A-Fa-f]+)',log)
    assert owner_masks and int(owner_masks[-1][0],16)==1
    audit_capture(out/(label+'.pcap'),cpus)
'''+effective_boot[b:]
namespace=dict(base.__dict__); namespace.update(audit=link_audit,link_set=link_set,audit_capture=link_wire_audit)
exec(compile(effective_boot,'<retained effective-link-boot.py>','exec'),namespace)
base.boot=namespace['boot']; base.baseline.fixture.run=probe_run
try: base.main()
finally:
    for endpoint in qmp_root.glob('*.sock'): endpoint.unlink()
    qmp_root.rmdir()
    if last_out:
        (last_out/'link-fixture-driver.py').write_text(Path(__file__).read_text())
        path=last_out/'manifest.json'
        if path.is_file():
            manifest=json.loads(path.read_text()); manifest['variant']='2C pending CONNECT/RECV/zero-window SEND carrier loss/recovery'
            if (last_out/'tree/bin/net-cpu-probe').is_file(): manifest['probe_elf_sha256']=base.baseline.fixture.digest(last_out/'tree/bin/net-cpu-probe')
            path.write_text(json.dumps(manifest,indent=2))
