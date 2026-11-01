#!/usr/bin/env python3
"""Ordinary multiple-package CLI on disposable installed roots; no physical I/O."""
import argparse,functools,hashlib,http.server,json,shutil,subprocess,tempfile,threading,time,uuid
from pathlib import Path
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
import pkg_build as app
import pkg_repository as repo
import test_pkg_upgrade_qemu as base
from test_root_phase2 import extract_root

def main():
    p=argparse.ArgumentParser();p.add_argument('--firmware',nargs='+',choices=['bios','uefi'],default=['bios','uefi']);p.add_argument('--cpus',type=int,default=4);p.add_argument('--port',type=int,default=18081);a=p.parse_args()
    root=base.ROOT;out=Path(tempfile.mkdtemp(prefix='multiple-',dir=root/'build/pkg-upgrade-qemu'));print('Evidence:',out,flush=True)
    key=Ed25519PrivateKey.generate();repository=bytes(range(16));trust='version=1\nrepository='+str(uuid.UUID(bytes=repository))+'\nroot_version=1\nplatform='+'42'*32+'\nbase_version=1\npublic_key='+repo.public(key).hex()+'\n'
    public=out/'repository';objects=public/'objects/sha256';objects.mkdir(parents=True)
    subprocess.run(['sh','examples/pkg/rot13/build.sh'],cwd=root,check=True)
    payload=(root/'build/pkg-rot13/rot13-v1.elf').read_bytes();artifacts={};executables={}
    for name,build in [('alpha',1),('alpha',2),('alpha',3),('beta',1),('beta',2),('extra',1)]:
        binary=payload+f'{name}:{build}'.encode();executables[name,build]=binary
        artifact=app.encode(name,name,str(build),bytes.fromhex('42'*32),build,1,1,[(f'bin/{name}',1,0o755,binary)])
        artifacts[name,build]=artifact;(objects/(hashlib.sha256(artifact).hexdigest()+'.fpk')).write_bytes(artifact)
    def publish(revision,pairs):
        now=int(time.time());wire,_=repo.catalog(repository,1,revision,min(now,1793448000)-86400,max(now,1793448000)+30*86400,[artifacts[x] for x in pairs],[key]);(public/'catalog.v1').write_bytes(wire)
    class Handler(http.server.SimpleHTTPRequestHandler):
        def log_message(self,format,*args):
            with (out/'http.log').open('a') as f:f.write(format%args+'\n')
    server=http.server.ThreadingHTTPServer(('127.0.0.1',a.port),functools.partial(Handler,directory=str(public)));thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    report={'result':'FAIL','scope':'disposable BIOS/UEFI installed roots; network HTTP with signed artifacts; no physical acceptance','cases':[]}
    try:
        template=base.prepare(root,out,trust,a.port)
        for firmware in a.firmware:
            publish(1,[('alpha',1),('beta',1),('extra',1)])
            disk=out/(firmware+'.img');shutil.copyfile(template,disk);g=base.Guest(out,disk,firmware,a.cpus,firmware,'kvm')
            try:
                g.login();g.root();g.command('/bin/pkg install alpha');g.command('/bin/pkg install beta')
                listing=g.command('/bin/pkg installed');assert 'alpha: build 1' in listing and 'beta: build 1' in listing and 'extra: build' not in listing
                assert 'beta: build 1' in g.command('/bin/pkg info beta')
                publish(2,[('alpha',2),('beta',1),('extra',1)])
                text=g.command('echo y | /bin/pkg upgrade',expected=1);assert 'noninteractive upgrade requires -y' in text
                start=len(g.raw);g.send('/bin/pkg upgrade; echo CANCEL:$?');g.wait(lambda t:'Proceed? [y/N]' in t,start);g.send('n');g.wait(lambda t:'CANCEL:1' in t and base.PROMPT.search(t),start)
                assert 'alpha: build 1' in g.command('/bin/pkg info alpha')
                text=g.command('/bin/pkg upgrade -y');assert 'alpha: completed' in text and 'beta: completed' not in text
                assert 'Installed packages are current' in g.command('/bin/pkg upgrade')
                publish(3,[('alpha',3),('beta',2),('extra',1)])
                missing=objects/(hashlib.sha256(artifacts['beta',2]).hexdigest()+'.fpk');retained=missing.read_bytes();missing.unlink()
                text=g.command('/bin/pkg upgrade -y',expected=1);assert 'alpha: completed' in text and 'beta: failed' in text
                assert 'alpha: build 3' in g.command('/bin/pkg info alpha');assert 'beta: build 1' in g.command('/bin/pkg info beta')
                missing.write_bytes(retained)
                start=len(g.raw);g.send('/bin/pkg upgrade; echo CONFIRMED:$?');g.wait(lambda t:'Proceed? [y/N]' in t,start);g.send('yes');text=g.wait(lambda t:'CONFIRMED:0' in t and base.PROMPT.search(t),start);assert 'beta: completed' in text
                publish(4,[('alpha',3),('extra',1)])
                text=g.command('/bin/pkg upgrade');assert 'beta: unavailable; left installed' in text and 'Installed packages are current' in text
                g.command('echo hello | /usr/bin/alpha');g.command('echo hello | /usr/bin/beta');g.shutdown()
            finally:g.close()
            g=base.Guest(out,disk,firmware,a.cpus,firmware+'-reboot','kvm')
            try:
                g.login();g.root();text=g.command('/bin/pkg installed');assert 'alpha: build 3' in text and 'beta: build 2' in text
                assert '\nuryyb\n' in g.command('echo hello | /usr/bin/alpha');assert '\nuryyb\n' in g.command('echo hello | /usr/bin/beta');g.shutdown()
            finally:g.close()
            image=out/(firmware+'.root.img');extract_root(disk,image);base.fixture.run(['e2fsck','-fn',str(image)],out/(firmware+'.fsck.log'))
            for slot,(name,build) in enumerate([('alpha',3),('beta',2)]):
                dest=out/(firmware+'.'+name);base.fixture.run(['debugfs','-R',f'dump /usr/bin/{name} {dest}',str(image)],out/'audit.log');assert dest.read_bytes()==executables[name,build]
                path='install.v2' if not slot else f'install-{slot:02}.v2';record=out/(firmware+'.'+path);base.fixture.run(['debugfs','-R',f'dump /var/lib/pkg/{path} {record}',str(image)],out/'audit.log');raw=record.read_bytes();assert len(raw)==8192 and int.from_bytes(raw[40:48],'little')==build and raw[320:352].rstrip(b'\0')==name.encode() and hashlib.sha256(raw[:8160]).digest()==raw[8160:]
            for name in ('passwd','group','shadow','pkg/trust.conf','pkg/repository.conf'):
                dest=out/(firmware+'.'+name.replace('/','-'));base.fixture.run(['debugfs','-R',f'dump /etc/{name} {dest}',str(image)],out/'audit.log');assert dest.read_bytes()==(out/'tree/etc'/name).read_bytes()
            report['cases'].append({'firmware':firmware,'smp':a.cpus,'result':'PASS','linux_fsck':'PASS','executables':'byte exact'})
        report['result']='PASS';print('PASS multiple installed-root package workflow:',out,flush=True)
    finally:
        server.shutdown();server.server_close();thread.join(timeout=10);(out/'manifest.json').write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
