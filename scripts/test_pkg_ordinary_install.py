"""Exercise the actual ordinary-root install callback on a disposable EXT4 image."""
import pathlib, subprocess, tempfile, shutil, contextlib
from pkg_install_fixture import fixture
from pkg_enroll_records import keys, PUB
ROOT=pathlib.Path(__file__).resolve().parents[1]
def main(upgrade=False):
    evidence=ROOT/'.codex-remote-attachments';evidence.mkdir(exist_ok=True)
    with contextlib.nullcontext(tempfile.mkdtemp(prefix='pkg-ordinary-',dir=evidence)) as td:
        print('Evidence directory:',td,flush=True)
        out=pathlib.Path(td)
        work=out/'work';work.mkdir()
        for name in ('src','user','tests'):
            shutil.copytree(ROOT/name,work/name)
        # Deterministic host-only clock; the signed fixture uses this epoch.
        p=work/'src/fs/pkg_executor.inc';s=p.read_text()
        assert 'now.tv_sec = 1730000000;' in s
        p.write_text(s.replace('now.tv_sec = 1730000000;','now.tv_sec = 1792929600;'))
        with tempfile.TemporaryDirectory(prefix='pkg-ordinary-test-keys-') as key_dir:
            paths=[]
            for i in (1,2,3):
                p=pathlib.Path(key_dir)/str(i);p.write_bytes(bytes([i])*32);p.chmod(0o600);paths.append(p)
            signers=keys(paths)
        image=fixture(out/'fixture',signers,bytes([7])*32,docs=0)
        trust='version=1\nrepository=04040404-0404-0404-0404-040404040404\nroot_version=3\nplatform='+bytes(range(32)).hex()+'\nbase_version=2\n'
        trust+=''.join('public_key='+PUB(k).hex()+'\n' for k in signers)
        (out/'trust').write_text(trust)
        flags=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-pthread','-DHOST_TEST','-DTOOL_HOST_TEST','-DFORTRESS_DAC_ENFORCED','-I.','-Itests','-Itests/ext4_host','-Itests/host','-Isrc/include','-Isrc/fs','-Isrc/drivers','-Isrc/mm','-Isrc/lib','-Isrc/kernel','-Isrc/arch/x86_64','-Isrc/net','-Iuser/tools','-Isrc/lib/crypto/upstream/src']
        def run(args,timeout=300):subprocess.run(args,cwd=work,check=True,timeout=timeout)
        objects=[]
        for i,src in enumerate(('src/lib/crypto/upstream/src/monocypher.c','src/lib/crypto/upstream/src/optional/monocypher-ed25519.c')):
            obj=out/f'mono{i}.o';run(flags+['-include','src/lib/crypto/namespace.h','-c',src,'-o',str(obj)]);objects.append(str(obj))
        units=['tests/pkg_ordinary_install_host.c','tests/ext4_fault_disk.c','src/fs/permission_values.c','src/kernel/creds.c','src/lib/account_redo.c','src/fs/pkg_plan.c','user/pkg/catalog.c','user/pkg/verify.c','user/pkg/format.c','user/pkg/manifest.c','user/pkg/repository.c','user/tools/gzip.c','user/tools/digest.c','src/lib/crypto/hash.c','src/lib/crypto/curve.c']
        binary=out/'ordinary';run(flags+units+objects+['-o',str(binary)])
        run([str(binary),str(image),str(out/'fixture/artifact'),str(out/'fixture/catalog'),str(out/'trust')])
        if upgrade:
            import pkg_build as app
            import pkg_repository as repo
            import hashlib
            subprocess.run(['sh','examples/pkg/rot13/build.sh'],cwd=ROOT,check=True)
            artifacts=[app.build_spec(ROOT/'examples/pkg/rot13'/f'v{i}.json') for i in (1,2)]
            executable=(ROOT/'build/pkg-rot13/rot13-v2.elf').read_bytes()+b'release-3'
            artifacts.append(app.encode('rot13','rot13','3.0',bytes.fromhex('42'*32),3,1,1,
                [('bin/rot13',1,0o755,executable)]+[(f'doc/{i:02}/README.txt',2,0o644,b'build three\n') for i in range(15)]))
            for i,artifact in enumerate(artifacts):
                (out/f'artifact{i}').write_bytes(artifact)
                wire,_=repo.catalog(bytes([4])*16,3,7+i,1792929500,1793029600,[artifact],signers)
                (out/f'catalog{i}').write_bytes(wire)
            (out/'hashes').write_bytes(b''.join(hashlib.sha256(x).digest() for x in
                [(ROOT/'build/pkg-rot13/rot13-v1.elf').read_bytes(),(ROOT/'build/pkg-rot13/rot13-v2.elf').read_bytes(),executable]))
            text='version=1\nrepository=04040404-0404-0404-0404-040404040404\nroot_version=3\nplatform='+'42'*32+'\nbase_version=1\n'
            text+=''.join('public_key='+PUB(k).hex()+'\n' for k in signers)
            (out/'upgrade-trust').write_text(text)
            units[0]='tests/pkg_upgrade_host.c';binary=out/'upgrade'
            run(flags+units+objects+['-o',str(binary)])
            run([str(binary),str(image),*[str(out/f'artifact{i}') for i in range(3)],
                 *[str(out/f'catalog{i}') for i in range(3)],str(out/'upgrade-trust'),str(out/'hashes'),str(out/'upgraded.img')],timeout=1800)
            run(['e2fsck','-fn',str(out/'upgraded.img')])
            full=out/'full.img';shutil.copyfile(out/'upgraded.img.installed',full)
            checked=subprocess.run(['e2fsck','-fy',str(full)],cwd=work,capture_output=True,text=True)
            assert checked.returncode in (0,1),checked.stdout+checked.stderr
            filler=out/'filler';filler.write_bytes(b'x'*(32*1024*1024))
            filled=subprocess.run(['debugfs','-w','-R',f'write {filler} /root/filler',str(full)],capture_output=True,text=True)
            (out/'fill.log').write_text(filled.stdout+filled.stderr)
            assert 'Could not allocate block' in filled.stderr,filled.stderr
            run(['e2fsck','-fn',str(full)])
            run([str(binary),str(full),*[str(out/f'artifact{i}') for i in range(3)],
                 *[str(out/f'catalog{i}') for i in range(3)],str(out/'upgrade-trust'),str(out/'hashes'),str(out/'unused.img'),'full'])


if __name__=='__main__':main()
