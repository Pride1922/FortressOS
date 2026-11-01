#!/usr/bin/env python3
"""Apply the new kernel to copies of the accepted B/rot13-2 roots, then add a package.

BIOS/UEFI; regular-file USB/NVMe only. Never modifies the preserved source roots.
"""
import argparse,functools,hashlib,http.server,json,shutil,subprocess,tempfile,threading,time
from pathlib import Path
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
import pkg_build as app
import pkg_repository as repo
import test_kernel_update as update
import test_pkg_upgrade_qemu as base
import test_rot13_installer as rot13
from test_root_phase2 import extract_root

def main():
    p=argparse.ArgumentParser();p.add_argument('--manifest',type=Path,required=True);p.add_argument('--source',type=Path,default=base.ROOT/'build/kernel-update/accept-8_eelmxd');p.add_argument('--port',type=int,default=18083);a=p.parse_args()
    record=json.loads(a.manifest.read_text());out=Path(tempfile.mkdtemp(prefix='pkg-multiple-update-',dir=base.ROOT/'build/kernel-update'));print('Evidence:',out,flush=True)
    public=out/'repository';objects=public/'objects/sha256';objects.mkdir(parents=True);shutil.copyfile(record['cli'],public/'pkg-next.elf')
    payload=(base.ROOT/'build/pkg-rot13/rot13-v1.elf').read_bytes();artifact=app.encode('second','second','1',bytes.fromhex('42'*32),1,1,1,[('bin/second',1,0o755,payload)])
    (objects/(hashlib.sha256(artifact).hexdigest()+'.fpk')).write_bytes(artifact);now=int(time.time());wire,_=repo.catalog(bytes(range(16)),1,100,min(now,1793448000)-86400,max(now,1793448000)+30*86400,[artifact],[Ed25519PrivateKey.from_private_bytes(bytes(range(32)))]);(public/'catalog.v1').write_bytes(wire)
    server=http.server.ThreadingHTTPServer(('127.0.0.1',a.port),functools.partial(http.server.SimpleHTTPRequestHandler,directory=str(public)));thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    result={'result':'FAIL','manifest':str(a.manifest),'physical_writes':False,'cases':[]};base.PASSWORD=rot13.PASSWORD
    try:
        for firmware in ('bios','uefi'):
            case=out/firmware;case.mkdir();source=a.source/firmware/'target.img'
            assert source.is_file() and not source.is_symlink(), 'source must be a retained regular-file fixture'
            before=base.fixture.digest(source);disk=case/'target.img';shutil.copyfile(source,disk)
            extract_root(disk,case/'before.root.img');legacy=case/'legacy.v2';base.fixture.run(['debugfs','-R',f'dump /var/lib/pkg/install.v2 {legacy}',str(case/'before.root.img')],case/'audit.log');assert int.from_bytes(legacy.read_bytes()[40:48],'little')==2
            update.update(case,disk,Path(record['update_media']),firmware)
            g=base.Guest(case,disk,firmware,4,'boot-c','kvm')
            try:
                update.login(g);g.root();assert '\nrot13 2.0\n' in g.command('/usr/bin/rot13 --version')
                g.command('/sbin/ifup 10.0.2.15/24 10.0.2.2 10.0.2.3')
                g.command(f'/bin/wget -q -O /home/operator/pkg-next http://10.0.2.2:{a.port}/pkg-next.elf');g.command('chmod 755 /home/operator/pkg-next')
                assert 'rot13: build 2' in g.command('/home/operator/pkg-next installed')
                g.command(f'echo version=1 > /etc/pkg/repository.conf');g.command(f'echo url=http://10.0.2.2:{a.port} >> /etc/pkg/repository.conf')
                g.command('/home/operator/pkg-next install second');assert '\nuryyb\n' in g.command('echo hello | /usr/bin/second');g.shutdown()
            finally:g.close()
            g=base.Guest(case,disk,firmware,4,'reboot-c','kvm')
            try:
                update.login(g);g.root();text=g.command('/home/operator/pkg-next installed');assert 'rot13: build 2' in text and 'second: build 1' in text
                assert '\nrot13 2.0\n' in g.command('/usr/bin/rot13 --version');assert '\nuryyb\n' in g.command('echo hello | /usr/bin/second');g.shutdown()
            finally:g.close()
            extract_root(disk,case/'after.root.img');actual=case/'after.v2';base.fixture.run(['debugfs','-R',f'dump /var/lib/pkg/install.v2 {actual}',str(case/'after.root.img')],case/'audit.log');assert actual.read_bytes()==legacy.read_bytes()
            base.fixture.run(['e2fsck','-fn',str(case/'after.root.img')],case/'fsck.log');assert base.fixture.digest(source)==before
            unchanged=base.fixture.digest(disk);update.update(case,disk,Path(record['update_media']),firmware,refuse=True);assert base.fixture.digest(disk)==unchanged
            result['cases'].append({'firmware':firmware,'smp':4,'result':'PASS','root_unchanged_by_kernel_update':True,'rot13_inventory_byte_exact':True,'second_package_and_reboot':'PASS','repeat_update_read_only_refusal':'PASS'})
        result['result']='PASS';print('PASS predecessor update / preserved rot13 / second-package install:',out,flush=True)
    finally:
        server.shutdown();server.server_close();thread.join(timeout=10);(out/'manifest.json').write_text(json.dumps(result,indent=2)+'\n')
if __name__=='__main__':main()
