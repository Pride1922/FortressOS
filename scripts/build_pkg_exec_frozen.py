"""Refresh private binding units in a new copy of a verified frozen build.

No production source, release output or retained input workspace is modified.
"""
import argparse,datetime,json,pathlib,shutil,subprocess,tarfile
import test_pkg_exec_boot as test

def main():
    parser=argparse.ArgumentParser();parser.add_argument('build',type=pathlib.Path);args=parser.parse_args();prior=args.build.resolve();assert prior.is_relative_to((test.ROOT/'.codex-remote-attachments').resolve())
    previous=json.loads((prior/'manifest.json').read_text());assert not previous.get('error');assert all(test.base.H(prior/'workspace'/p)==h for p,h in previous['compiled_inputs'].items())
    out=test.ROOT/'.codex-remote-attachments'/('pkg-exec-frozen-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'));out.mkdir();work=out/'workspace';shutil.copytree(prior/'workspace',work)
    m={'derived_from':str(prior),'commands':[],'compiled_inputs':{},'sources':dict(previous['sources']),'guards':dict(previous['guards']),'build':'INCOMPLETE'}
    def save():(out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n')
    def run(argv,label):
        m['commands'].append(argv);save();r=subprocess.run(argv,cwd=work,capture_output=True,text=True,timeout=300);(out/(label+'.log')).write_text(r.stdout+r.stderr);assert not r.returncode,(label,r.returncode,r.stderr[-4000:])
    try:
        for name in ('tests/pkg_exec_kernel.inc','tests/pkg_exec_ring3.c','tests/pkg_exec_wire.h','tests/pkg_exec_cli.c','tests/pkg_exec_cli.h'):
            shutil.copyfile(test.ROOT/name,work/name);m['sources'][name]=test.base.H(test.ROOT/name)
        generated=work/'src/kernel/pkg_boot_private.c';before=generated.read_text();after=before.replace('if(t->pkg_owner.generation)gk_check(pw_exec(gk_key(t)))','if(t&&t->pkg_owner.generation)gk_check(pw_exec(gk_key(t)))');generated.write_text(after)
        m['generated_owner_null_guard']={'file':'src/kernel/pkg_boot_private.c','before_sha256':test.H(before.encode()).hex(),'after_sha256':test.base.H(generated)}
        m['sources']['scripts/pkg_exec_overlay.py']=test.base.H(test.ROOT/'scripts/pkg_exec_overlay.py')
        run(['make','-j8','bin/fortress.elf'],'kernel')
        flags=['gcc','-std=c11','-Os','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-fno-pie','-fno-stack-protector','-mno-red-zone','-mno-sse','-mno-mmx','-msoft-float','-Wframe-larger-than=1024','-I.','-Isrc/include','-Itests']
        run(flags+['-c','tests/pkg_exec_ring3.c','-o','build/pkgboot.o'],'probe')
        objects=[work/p for p in test.cli_objects(run)]
        run(['ld','-nostdlib','-static','-z','noexecstack','--gc-sections','-T','user/shell.ld','build/pkgboot-start.o','build/pkgboot.o',*[str(p) for p in objects],'-o','build/pkgboot.elf'],'link')
        with tarfile.open(prior/'bootstrap.tar') as src,tarfile.open(out/'bootstrap.tar','w',format=tarfile.USTAR_FORMAT) as dst:
            for member in src.getmembers():
                if member.name.lstrip('./')=='bin/pkg-boot':continue
                dst.addfile(member,src.extractfile(member) if member.isfile() else None)
            dst.add(work/'build/pkgboot.elf',arcname='bin/pkg-boot')
        assert (out/'bootstrap.tar').stat().st_size<=8*1024*1024
        frames={line.split('\t')[0]:int(line.split('\t')[1]) for line in (work/'build/fs/ext4.su').read_text().splitlines() if any(':'+p in line for p in ('px_','mx_','jb_boot'))};assert frames and max(frames.values())<=1024
        m['frames']=frames;m['compiled_inputs']={str(p.relative_to(work)):test.base.H(p) for p in work.rglob('*') if p.is_file() and p.suffix in ('.c','.h','.inc','.asm')};m['kernel_sha256']=test.base.H(work/'bin/fortress.elf');m['bootstrap_sha256']=test.base.H(out/'bootstrap.tar');m['build']='PASS'
    except BaseException as e:m['build']='FAIL';m['error']=repr(e);raise
    finally:save();print(out,m['build'],flush=True)
if __name__=='__main__':main()
