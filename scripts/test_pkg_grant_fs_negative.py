"""Read-only refinement refusals on actual EXT4 and private injected bytes."""
import datetime
import hashlib
import json
import pathlib
import subprocess
from test_ext4_time_host import fixture

ROOT=pathlib.Path(__file__).resolve().parent.parent


def main():
    out=ROOT/'.codex-remote-attachments'/('pkg-grant-fs-negative-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    out.mkdir(parents=True,exist_ok=False)
    names=[p for directory in ('src','user','tests','scripts') for p in (ROOT/directory).rglob('*')
           if p.is_file() and p.suffix in ('.c','.h','.inc','.py','.mk') and '__pycache__' not in p.parts]
    names += [ROOT/'AGENTS.md',ROOT/'README.md']
    sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    sources={str(p.relative_to(ROOT)):sha(p) for p in names}
    manifest={'result':'INCOMPLETE','sources':sources,'cases':[],'commands':[],
        'scope':'private actual EXT4 refinement; zero-write refusals after fixture preparation/injection; not production mount admission'}
    def run(cmd,label):
        manifest['commands'].append(cmd)
        r=subprocess.run(cmd,text=True,capture_output=True,cwd=ROOT,timeout=180)
        with (out/(label+'.log')).open('a') as stream:stream.write(r.stdout+r.stderr)
        assert r.returncode==0,(label,r.returncode,r.stdout[-1500:],r.stderr[-1500:])
        return r.stdout
    try:
        # Compose a separate entry point without changing the positive campaign's
        # source while it runs. Generated C is retained as exact build evidence.
        source=(ROOT/'tests/pkg_grant_fs_host.c').read_text()
        assert source.count('int main(int argc,char **argv){')==1
        source=source.replace('int main(int argc,char **argv){','int pg_positive_main(int argc,char **argv){')
        source+='\n#include "pkg_grant_fs_negative.inc"\n'+r'''
int main(int argc,char **argv){
    assert(argc==4||argc==5);size_t size;uint8_t *initial=load(argv[1],&size);unsigned ss=(unsigned)strtoul(argv[2],NULL,10);
    ext4_fault_disk_t d={.stable=malloc(size),.volatile_bytes=malloc(size),.bytes=size,.cut=-1};assert(d.stable&&d.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=size/ss,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    ext4_mount_t *fs;reset(&d,initial);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    if(argc==5){size_t events=d.events;assert(!eligible(fs)&&events==d.events);puts("PASS single-group eligibility refusal writes=0");
        assert(!ext4_freeze_and_sync(fs));teardown();free(initial);free(d.stable);free(d.volatile_bytes);return 0;}
    seed(fs);
    assert(!change(fs,false));check(fs);negative_tests(fs);check(fs);
    assert(!ext4_freeze_and_sync(fs));save(argv[3],"negative-restored",d.stable,size);
    teardown();free(initial);free(d.stable);free(d.volatile_bytes);return 0;
}
'''
        generated=out/'negative.c';generated.write_text(source)
        cmd=['gcc','-std=c11','-O1','-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror',
             '-no-pie','-pthread','-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
        cmd+=['-I'+p for p in ('tests','tests/ext4_host','tests/host','src/include','src/fs','src/drivers','src/mm','user/tools','.')]
        cmd += [str(generated),'tests/pkg_grant_candidate.c','tests/ext4_fault_disk.c',
                'src/fs/permission_values.c','src/kernel/creds.c','user/tools/digest.c','-o',str(out/'probe')]
        dependency=run(cmd[:cmd.index('-o')]+['-MM','-MT','probe'],'dependencies')
        deps={str((ROOT/p).resolve().relative_to(ROOT)) for p in dependency.replace('\\\n',' ').split() if not p.endswith(':')}
        manifest['compiled_inputs']={p:sha(ROOT/p) for p in sorted(deps)}
        run(cmd,'compile')
        for bs in (1024,2048,4096):
            image=fixture(out,bs);digest=sha(image)
            for ss in (512,4096):
                prefix=out/f'{bs}-{ss}'
                text=run([str(out/'probe'),str(image),str(ss),str(prefix)],f'{bs}-{ss}')
                assert 'negative-refinements=18 read-only-refusals' in text
                run(['e2fsck','-fn',str(prefix)+'-negative-restored.img'],f'{bs}-{ss}-linux')
                manifest['cases'].append({'block':bs,'sector':ss,'zero_write_refusals':18,'linux_audits':1,'result':'PASS'})
                print(manifest['cases'][-1],flush=True)
            assert sha(image)==digest
        small=out/'single-group';small.mkdir();image=fixture(small,4096,inodes=128,size_mib=16)
        for ss in (512,4096):run([str(out/'probe'),str(image),str(ss),str(out/f'single-{ss}'),'profile-refusal'],f'single-{ss}')
        manifest['single_group_refusals']=2
        manifest['result']='PASS'
    finally:
        manifest['changed_sources']=[p for p,h in sources.items() if sha(ROOT/p)!=h]
        manifest['changed_compiled_inputs']=[p for p,h in manifest.get('compiled_inputs',{}).items() if sha(ROOT/p)!=h]
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    assert not manifest['changed_compiled_inputs']
    print('Evidence:',out)


if __name__=='__main__':main()
