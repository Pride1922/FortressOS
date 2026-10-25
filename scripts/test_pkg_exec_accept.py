"""Repeat acceptance using a hash-checked private kernel/probe build.

Creates a fresh disposable enrollment and ephemeral MAC key. Never modifies
the retained compiled workspace, production media or an existing target root.
"""
import argparse,datetime,json,pathlib,tempfile,shutil,struct
import test_pkg_exec_boot as test
from pkg_enroll_records import keys,H
from pkg_enroll_fixture import command,gpt

def main():
    parser=argparse.ArgumentParser();parser.add_argument('build',type=pathlib.Path);parser.add_argument('--smoke',action='store_true');args=parser.parse_args()
    prior=args.build.resolve();assert prior.is_relative_to((test.ROOT/'.codex-remote-attachments').resolve())
    previous=json.loads((prior/'manifest.json').read_text());assert not previous.get('error')
    work=prior/'workspace';assert all(test.base.H(work/p)==h for p,h in previous['compiled_inputs'].items())
    out=test.ROOT/'.codex-remote-attachments'/('pkg-exec-accept-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'));out.mkdir()
    m={'build':str(prior),'boot_runtime':'INCOMPLETE','preservation':'INCOMPLETE','cases':[],'commands':[],'source_changes':[]}
    source_hashes={p:test.base.H(test.ROOT/p) for p in previous['sources'] if (test.ROOT/p).exists()}
    source_hashes['scripts/test_pkg_exec_accept.py']=test.base.H(pathlib.Path(__file__))
    guard_hashes={p:test.base.H(test.ROOT/p) for p in previous['guards'] if (test.ROOT/p).is_file()}
    m['source_snapshot']=source_hashes;m['guard_snapshot']=guard_hashes;m['build_manifest_sha256']=test.base.H(prior/'manifest.json')
    matched=('tests/pkg_exec_kernel.inc','tests/pkg_exec_ring3.c','tests/pkg_exec_wire.h','tests/pkg_install_fs.inc','tests/pkg_plan_candidate.c','user/pkg/mutation_cli.c')
    assert all(test.base.H(test.ROOT/p)==test.base.H(work/p) for p in matched)
    m['matched_current_implementation']={p:test.base.H(work/p) for p in matched}
    def save():(out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n')
    def run(argv,label):m['commands'].append(argv);save();(out/(label+'.log')).write_text(command(argv))
    try:
        with tempfile.TemporaryDirectory(prefix='fortress-pkgexec-accept-key-') as temp:
            temp=pathlib.Path(temp);import os
            test.KEY=os.urandom(32);m['local_mac_key_fingerprint']=H(test.KEY).hex();key=temp/'key';key.write_bytes(test.KEY);key.chmod(0o600);roles={}
            for name,values in (('selector',(111,112,113)),('catalog',(1,2,3))):
                roles[name]=[]
                for value in values:
                    p=temp/f'{name}-{value}';p.write_bytes(bytes([value])*32);p.chmod(0o600);roles[name].append(p)
                keys(roles[name])
            shutil.copyfile(prior/'bootstrap.tar',out/'bootstrap.tar');test.BOOTSTRAP=out/'bootstrap.tar'
            root=out/'initial.root';prepared=test.build(root,out/'records',roles,bytes.fromhex(test.base.H(work/'bin/fortress.elf')),H((out/'bootstrap.tar').read_bytes()))
            (out/'policy.bin').write_bytes(prepared['policy']);(out/'selector.bin').write_bytes(prepared['selector']);(out/'enrolled.img').write_bytes(gpt(root.read_bytes()));shutil.copyfile(out/'records/executor/expected.json',out/'expected.json');run(['e2fsck','-fn',str(root)],'initial-fsck')
            media=out/'media';(media/'boot/limine').mkdir(parents=True);(media/'EFI/BOOT').mkdir(parents=True)
            for name,p in (('fortress.elf',work/'bin/fortress.elf'),('bootstrap.tar',out/'bootstrap.tar'),('policy.bin',out/'policy.bin'),('selector.bin',out/'selector.bin')):shutil.copyfile(p,media/'boot'/name)
            for name in ('limine-bios.sys','limine-bios-cd.bin','limine-uefi-cd.bin'):shutil.copyfile(work/'limine'/name,media/'boot/limine'/name)
            shutil.copyfile(work/'limine/BOOTX64.EFI',media/'EFI/BOOT/BOOTX64.EFI')
            (media/'boot/limine/limine.conf').write_text('timeout: 0\n/Private executor acceptance\n    protocol: limine\n    kernel_path: boot():/boot/fortress.elf\n    kernel_cmdline: boot_mode=pkg-exec-v1\n    module_path: boot():/boot/bootstrap.tar\n    module_cmdline: role=bootstrap\n    module_path: boot():/boot/selector.bin\n    module_cmdline: role=pkg-selector\n    module_path: boot():/boot/policy.bin\n    module_cmdline: role=pkg-policy\n')
            iso=out/'valid.iso';run(['xorriso','-as','mkisofs','-b','boot/limine/limine-bios-cd.bin','-no-emul-boot','-boot-load-size','4','-boot-info-table','--efi-boot','boot/limine/limine-uefi-cd.bin','--efi-boot-part','--efi-boot-image','--protective-msdos-label',str(media),'-o',str(iso)],'iso');run([str(work/'limine/limine'),'bios-install',str(iso)],'bios-install')
            assert test.KEY not in iso.read_bytes();m['measurements']={p:test.base.H(out/p) for p in ('valid.iso','bootstrap.tar','policy.bin','selector.bin')};m['kernel_sha256']=test.base.H(work/'bin/fortress.elf')
            for fw,smp in ([('bios',1)] if args.smoke else [('bios',1),('bios',4),('uefi',1),('uefi',4)]):
                label=f'{fw}-{smp}';m['cases'].append(test.boot(out,work,iso,out/'enrolled.img',fw,smp,label+'-cut',key,cut=True));save()
                m['cases'].append(test.boot(out,work,iso,out/(label+'-cut')/'disk.img',fw,smp,label,key));save()
                m['cases'].append(test.boot(out,work,iso,out/label/'disk.img',fw,smp,label+'-reboot',key));save()
            wrong=temp/'wrong';wrong.write_bytes(os.urandom(32));wrong.chmod(0o600);m['cases'].append(test.boot(out,work,iso,out/'enrolled.img','bios',1,'wrong-key',wrong,True));save()
            foreign=out/'foreign.root';shutil.copyfile(root,foreign);payload=out/'foreign.bytes';payload.write_bytes(b'foreign stage\n');commands=out/'foreign.commands';commands.write_text(f'write {payload} /var/lib/fortress/accounts/passwd.new\nset_inode_field /var/lib/fortress/accounts/passwd.new mode 0100600\nset_inode_field /var/lib/fortress/accounts/passwd.new uid 0\nset_inode_field /var/lib/fortress/accounts/passwd.new gid 0\n')
            run(['debugfs','-w','-f',str(commands),str(foreign)],'foreign-seed');test.canonical_account_stage(foreign);run(['e2fsck','-fn',str(foreign)],'foreign-fsck');target=out/'foreign.img';target.write_bytes(gpt(foreign.read_bytes()));m['cases'].append(test.boot(out,work,iso,target,'bios',1,'foreign-account',key,True));m['boot_runtime']='PASS'
            from pkg_artifact_vectors import elf
            for label,path in (('foreign-executable','/usr/bin/unowned'),('foreign-transaction','/pkg/transaction.v1/stale')):
                foreign=out/(label+'.root');shutil.copyfile(root,foreign);payload=out/(label+'.bytes');payload.write_bytes(elf());commands=out/(label+'.commands');commands.write_text(f'write {payload} {path}\nset_inode_field {path} mode 0100755\nset_inode_field {path} uid 0\nset_inode_field {path} gid 0\n')
                run(['debugfs','-w','-f',str(commands),str(foreign)],label+'-seed');run(['e2fsck','-fn',str(foreign)],label+'-fsck');target=out/(label+'.img');target.write_bytes(gpt(foreign.read_bytes()));m['cases'].append(test.boot(out,work,iso,target,'bios',1,label,key,True));save()
    except BaseException as e:m['boot_runtime']='FAIL';m['error']=repr(e);raise
    finally:
        m['changed_copies']=[p for p,h in previous['compiled_inputs'].items() if test.base.H(work/p)!=h];m['source_changes']=[p for p,h in source_hashes.items() if test.base.H(test.ROOT/p)!=h];m['changed_guards']=[p for p,h in guard_hashes.items() if test.base.H(test.ROOT/p)!=h];m['preservation']='FAIL' if m['changed_copies'] or m['source_changes'] or m['changed_guards'] else 'PASS';save();print(out,m['boot_runtime'],m['preservation'],flush=True)
if __name__=='__main__':main()
