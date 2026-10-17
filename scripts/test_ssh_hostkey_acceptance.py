#!/usr/bin/env python3
"""Real Ring 3 install/host-identity acceptance on temporary regular NVMe disks.
Only public identity and explicitly allowlisted diagnostics survive cleanup.
Failure injection is confined to source snapshots, never the release checkout.
"""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import tarfile
import tempfile
import threading
import test_root_phase0 as fixture
import test_root_phase2 as boots
import test_installer as installer
import create_installer_media as media_builder

ROOT=Path(__file__).resolve().parent.parent
OPERATOR='hostkey-acceptance-operator'
RECOVERY='hostkey-independent-recovery'
LOCK=threading.Lock()

def run(argv,cwd=ROOT,timeout=600):
    return subprocess.run(list(map(str,argv)),cwd=cwd,check=True,capture_output=True,text=True,timeout=timeout).stdout

def sources():
    return {str(p.relative_to(ROOT)):fixture.digest(p) for base in ('src','user','scripts','tests')
            for p in (ROOT/base).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.inc','.py','.asm','.ld')}

def prepare(profile,out):
    workspace=Path(run(['python3','scripts/create_ext4_guest_workspace.py']).strip())
    changes={}
    if profile!='ready':
        source=workspace/'src/kernel/random.c';before=source.read_text()
        signature='static unsigned random_hw_features(void) {' if profile=='absent' else 'static bool random_hw_word(bool seed, uint64_t *out) {'
        injected='return 0;' if profile=='absent' else '(void)seed;(void)out;return false;'
        assert before.count(signature)==1
        after=before.replace(signature,signature+'\n    /* ISOLATED HOSTKEY ACCEPTANCE: fail-only hardware adapter. */\n    '+injected,1)
        source.write_text(after)
        changes={'src/kernel/random.c':{'before':hashlib.sha256(before.encode()).hexdigest(),
                                     'after':fixture.digest(source),'failure':profile}}
    command=['make','-j8','INSTALLER_TEST=1','HOSTKEY_TEST_PROBE=1','all','build/tool-install.elf']
    result=subprocess.run(command,cwd=workspace,stdout=(out/(profile+'-build.log')).open('w'),stderr=subprocess.STDOUT,timeout=600)
    assert not result.returncode
    templates=Path(run(['python3','scripts/create_installer_payload.py'],cwd=workspace).strip().splitlines()[-1])
    media_builder.ROOT=workspace
    media=media_builder.build(templates,workspace/'bin/fortress.elf',workspace/'bin/initramfs.tar',workspace/'build/tool-install.elf')
    with tarfile.open(workspace/'bin/initramfs.tar') as archive:
        assert 'bin/hostkey-probe' in archive.getnames()
        assert not any(Path(n).name in ('ssh_host_ed25519_key','.fortress-hostkey.pending') for n in archive.getnames())
    protected=[workspace/'bin/fortress.elf',workspace/'bin/initramfs.tar',workspace/'build/tool-install.elf',
               *(templates/name for name in ('root.img','esp.img','bios.img','mbr.bin','payload.v1')),
               media/'fortress-installer.iso',media/'fortress-installer.img']
    return {'workspace':str(workspace),'media':str(media),'overrides':changes,'build':command,
            'immutable':{str(p):fixture.digest(p) for p in protected}}

def session(case,disk,firmware,profile,boot,manifest):
    def hook(number,password,send,wait,command,prompt,transcript):
        command('/bin/hostkey-probe unprivileged','HOSTKEY boundary PASS')
        def privileged(cmd,marker):
            start=len(transcript);send('sudo '+cmd)
            wait(lambda t:t.endswith('sudo password: '),start);send(password)
            wait(lambda t:marker in t and prompt(t),start)
            return transcript[start:].decode(errors='replace')
        mode='present' if profile=='ready' else 'absent'
        privileged('/bin/hostkey-probe '+mode,'HOSTKEY boundary PASS')
        if profile=='ready':
            text=privileged('/sbin/ssh-keygen --fingerprint','SHA256:')
            values=re.findall(r'SHA256:[A-Za-z0-9+/]{43}',text);assert len(values)==1
            with LOCK:
                prior=manifest.setdefault('fingerprints',{}).setdefault(firmware,values[0]);assert prior==values[0]
            assert values[0] in privileged('/sbin/ssh-keygen --provision','SHA256:')
        else:
            privileged('/sbin/ssh-keygen --provision','strong random/key generation failed')
            command('echo HOSTKEY_STATUS=$?','HOSTKEY_STATUS=1')
            privileged('/bin/hostkey-probe absent','HOSTKEY boundary PASS')
        return password
    # Both threads share this identical hook body through one global function;
    # its per-case parameters are supplied via thread-local routing below.
    LOCAL.hook=hook
    return boots.guest(case,disk,firmware,1,boot,OPERATOR,RECOVERY,manifest,accel='kvm')

LOCAL=threading.local()
def route(*args):return LOCAL.hook(*args)

def case_run(native,out,firmware,profile,prepared,manifest):
    case=native/(profile+'-'+firmware);case.mkdir()
    installer.ROOT=Path(prepared['workspace']);installer.ACCEL='kvm'
    disk=installer.install(case,Path(prepared['media']),firmware,384,manifest,operator=OPERATOR,recovery=RECOVERY,hostname='ssh-acceptance')
    serial=(case/'installer-serial.log').read_text(errors='replace')
    warning='SSH host identity unavailable.'
    assert (warning in serial)==(profile!='ready')
    for boot in range(3):session(case,disk,firmware,profile,boot,manifest)
    root=case/'oracle-root.img';boots.extract_root(disk,root)
    run(['e2fsck','-fn',root])
    audit={'firmware':firmware,'profile':profile,'result':'PASS','warning':warning in serial}
    if profile=='ready':
        for path,mode in [('/etc/ssh','0700'),('/etc/ssh/ssh_host_ed25519_key','0600')]:
            value=run(['debugfs','-R','stat '+path,root]);assert 'Mode:  '+mode in value and 'User:     0' in value and 'Group:     0' in value
        private=case/'oracle-private';run(['debugfs','-R',f'dump /etc/ssh/ssh_host_ed25519_key {private}',root]);private.chmod(0o600)
        public=run(['ssh-keygen','-y','-f',private]);pub=case/'public-key.pub';pub.write_text(public)
        fingerprint=re.search(r'SHA256:[A-Za-z0-9+/]{43}',run(['ssh-keygen','-lf',pub]))[0]
        assert fingerprint==manifest['fingerprints'][firmware];audit['fingerprint']=fingerprint
        private.write_bytes(bytes(private.stat().st_size));private.unlink()
    else:
        # debugfs exits zero even on missing names; assert diagnostics and that
        # no inode was returned for either canonical or staging entry.
        for name in ('ssh_host_ed25519_key','.fortress-hostkey.pending'):
            result=subprocess.run(['debugfs','-R','stat /etc/ssh/'+name,str(root)],capture_output=True,text=True,check=True)
            assert 'File not found' in result.stderr and 'Inode:' not in result.stdout
        audit['canonical_and_pending_absent']=True
    assert all(fixture.digest(Path(p))==h for p,h in prepared['immutable'].items())
    with LOCK:manifest['audits'].append(audit)
    print('PASS hostkey install/reboot/boundaries/oracle '+profile+' '+firmware,flush=True)

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--profiles',default='ready,absent,stall');parser.add_argument('--prepared',type=Path);args=parser.parse_args()
    profiles=args.profiles.split(',');assert profiles and set(profiles)<=set(('ready','absent','stall'))
    assert os.access('/dev/kvm',os.R_OK|os.W_OK),'KVM required for bounded real-installer campaign'
    build=ROOT/'build/ssh-hostkey-acceptance';build.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='gate-',dir=build));before=sources()
    release={str(p):fixture.digest(p) for p in (ROOT/'bin').glob('*') if p.is_file()}
    manifest={'status':'FAIL','head':run(['git','rev-parse','HEAD']).strip(),'cases':[],'audits':[],'prepared':{},
              'scope':'actual Ring 3 installer/ABI; failure-source adapters only in labelled snapshots; no physical/daemon/rotation acceptance'}
    try:
        old=json.loads(args.prepared.read_text())['prepared'] if args.prepared else {}
        for profile in profiles:
            prepared=old.get(profile) or prepare(profile,out);manifest['prepared'][profile]=prepared
            assert all(fixture.digest(Path(p))==h for p,h in prepared['immutable'].items())
            (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
            with tempfile.TemporaryDirectory(prefix='fortress-hostkey-acceptance-') as temporary:
                native=Path(temporary)
                try:
                    boots.SESSION_HOOK=route
                    with ThreadPoolExecutor(max_workers=2) as pool:
                        futures=[pool.submit(case_run,native,out,fw,profile,prepared,manifest) for fw in ('bios','uefi')]
                        for future in futures:future.result()
                    if profile=='ready':assert len(set(manifest['fingerprints'].values()))==2
                finally:
                    boots.SESSION_HOOK=None
                    for path in native.rglob('*'):
                        if path.is_file() and (path.name.endswith(('-argv.json','-serial.log','-stderr.log','-fsck.log')) or path.name=='public-key.pub'):
                            dest=out/path.relative_to(native);dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,dest)
        assert sources()==before,'source checkout changed during campaign'
        assert all(fixture.digest(Path(p))==h for p,h in release.items()),'release artifact changed'
        manifest.update(status='PASS',sources=before,release_preserved=release)
    finally:
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');print(out,flush=True)
if __name__=='__main__':main()
