"""Build desktop FPKs and provision a NEW private installer root with real records.

Factory authority is the immutable installer image, not a fabricated signed
catalog. No private keys or host adapters enter the media. Runtime installation
and upgrades retain their ordinary signature checks.
"""
import hashlib
import io
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tarfile
import tempfile
import tomllib
from build_desktop_package import APPS, build_app
from create_installer_media import ROOT, digest

ORDER = tuple(sorted(APPS, key=lambda app: APPS[app][0]))
FLAGS = ['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
         '-fno-omit-frame-pointer','-no-pie','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
         '-pthread','-DHOST_TEST','-DTOOL_HOST_TEST','-DFORTRESS_DAC_ENFORCED','-I.','-Itests','-Itests/ext4_host',
         '-Itests/host','-Isrc/include','-Isrc/fs','-Isrc/drivers','-Isrc/mm','-Isrc/lib','-Isrc/kernel',
         '-Isrc/arch/x86_64','-Isrc/net','-Iuser/tools','-Isrc/lib/crypto/upstream/src']
UNITS = ['tests/pkg_factory_seed_host.c','tests/ext4_fault_disk.c','src/fs/permission_values.c','src/kernel/creds.c',
         'src/lib/account_redo.c','src/lib/install_expand.c','src/fs/pkg_plan.c','user/pkg/catalog.c','user/pkg/verify.c','user/pkg/format.c',
         'user/pkg/manifest.c','user/pkg/repository.c','user/tools/gzip.c','user/tools/digest.c',
         'src/lib/crypto/hash.c','src/lib/crypto/curve.c']


def run(args, log, cwd=ROOT, timeout=300):
    with Path(log).open('ab') as stream:
        subprocess.run(list(map(str,args)),cwd=cwd,stdout=stream,stderr=subprocess.STDOUT,check=True,timeout=timeout)


def build_packages(out, sequence=1, abi=1, platform=bytes.fromhex('42'*32)):
    out=Path(out);out.mkdir(parents=True,exist_ok=True)
    version=tomllib.loads((ROOT/'user/compositor/Cargo.toml').read_text())['package']['version']
    records=[]
    for app in ORDER:
        name, source, _ = APPS[app]
        data=build_app(app,ROOT/source,platform,sequence,abi,abi,version)
        path=out/(name+'.fpk');path.write_bytes(data)
        mn=struct.unpack_from('<I',data,20)[0]
        import gzip
        with tarfile.open(fileobj=io.BytesIO(gzip.decompress(data[128+mn:]))) as tar:
            executable=tar.extractfile('bin/'+app).read()
        records.append({'name':name,'command':app,'build':sequence,'version':version,'path':str(path.resolve()),
                        'sha256':hashlib.sha256(data).hexdigest(),'executable_sha256':hashlib.sha256(executable).hexdigest()})
    manifest={'authority':'factory installer image; no repository catalog/signing key fabricated','platform':platform.hex(),
              'base_abi':abi,'packages':records}
    (out/'bundle.json').write_text(json.dumps(manifest,indent=2)+'\n');return manifest


def host_tool():
    parent=ROOT/'build/desktop-package-host';parent.mkdir(parents=True,exist_ok=True)
    inputs={str(p.relative_to(ROOT)):digest(p) for folder in ('src','user','tests') for p in (ROOT/folder).rglob('*')
            if p.is_file() and 'target' not in p.parts and p.suffix in ('.c','.h','.inc')}
    # Bind private fixture changes and flags as well as all included C headers.
    key=hashlib.sha256(json.dumps([inputs,FLAGS,digest(Path(__file__)),subprocess.check_output(['gcc','--version'],text=True)],sort_keys=True).encode()).hexdigest()
    out=parent/key;binary=out/'factory-seed'
    if binary.is_file():return binary,out
    out.mkdir(exist_ok=True);work=out/'work';work.mkdir(exist_ok=True)
    for name in ('src','user','tests'):
        shutil.copytree(ROOT/name,work/name,ignore=shutil.ignore_patterns('target','__pycache__'),dirs_exist_ok=True)
    # Preserve the installer's two groups of descriptor-growth padding. The
    # unchanged expansion code and an independent Linux audit check the result.
    source=work/'src/fs/ext4_mutate.inc';text=source.read_text()
    needle='for (uint32_t bit=0;bit<count;bit++) if (!e4_bit(map,bit)) {'
    assert text.count(needle)==1
    source.write_text(text.replace(needle,needle+'\n            if (g<2 && bit>=2 && bit<=64) continue;'))
    objects=[]
    for i,name in enumerate(('src/lib/crypto/upstream/src/monocypher.c','src/lib/crypto/upstream/src/optional/monocypher-ed25519.c')):
        obj=out/f'mono-{i}.o';run(FLAGS+['-include','src/lib/crypto/namespace.h','-c',name,'-o',obj],out/'compile.log',work);objects.append(obj)
    pending=out/'factory-seed.new';run(FLAGS+UNITS+objects+['-o',pending],out/'compile.log',work);pending.replace(binary)
    (out/'manifest.json').write_text(json.dumps({'sources':inputs,'private_allocator_change':'reserve seed descriptor-growth padding',
                                               'binary_sha256':digest(binary),'flags':FLAGS},indent=2)+'\n')
    return binary,out


def audit_root(image,templates,bundle,out):
    log=out/'audit.log';run(['e2fsck','-fn',image],log)
    for name in ('passwd','group','shadow','pkg/trust.conf','pkg/repository.conf','desktop.conf'):
        dumped=out/('preserved-'+name.replace('/','-'))
        run(['debugfs','-R',f'dump /etc/{name} {dumped}',image],log)
        assert dumped.read_bytes()==(templates/'tree/etc'/name).read_bytes(),name
    for slot,p in enumerate(bundle['packages']):
        for source,expected,label in ((f"/usr/bin/{p['command']}",p['executable_sha256'],'elf'),
                (f"/var/lib/pkg/sources/{p['sha256'][:8]}/{p['sha256'][8:]}.fpk",p['sha256'],'source')):
            dumped=out/f"{p['name']}-{label}";run(['debugfs','-R',f'dump {source} {dumped}',image],log)
            assert digest(dumped)==expected,source
        record=out/f"{p['name']}-record"
        source='/var/lib/pkg/'+('install.v2' if not slot else f'install-{slot:02}.v2')
        run(['debugfs','-R',f'dump {source} {record}',image],log);raw=record.read_bytes()
        assert len(raw)==8192 and hashlib.sha256(raw[:8160]).digest()==raw[8160:]
        assert raw[320:352].rstrip(b'\0').decode()==p['name'] and raw[352:384].rstrip(b'\0').decode()==p['command']
        assert struct.unpack_from('<QQQ',raw,40)==(p['build'],0,0)
        assert raw[112:144].hex()==p['sha256']
    return True


def seed(templates,bundle,out,upgrade_inputs=()):
    templates=Path(templates);out=Path(out)
    if templates.is_symlink() or not templates.resolve().is_relative_to((ROOT/'build/root-phase4').resolve()):
        raise ValueError('factory templates must be private build/root-phase4 files')
    if out.is_symlink() or not out.resolve().is_relative_to((ROOT/'build').resolve()):
        raise ValueError('factory output must be private build files')
    if (templates/'root.img').is_symlink() or not (templates/'root.img').is_file():raise ValueError('regular root template required')
    out.mkdir(parents=True,exist_ok=True)
    binary,tool_out=host_tool();image=out/'seeded-root.img'
    run([binary,templates/'root.img',image,*[p['path'] for p in bundle['packages']],*upgrade_inputs],out/'seed.log',timeout=600)
    audit_root(image,templates,bundle,out)
    probe=out/'expand-probe.img';shutil.copyfile(image,probe);expand=out/'expand-host'
    run(['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-no-pie',
         '-Itests/host','-Isrc/include','tests/install_expand_host.c','src/lib/install_expand.c','-o',expand],out/'expand.log')
    run([expand,probe,str(320*1024*1024)],out/'expand.log');run(['e2fsck','-fn',probe],out/'expand.log');probe.unlink()
    image.replace(templates/'root.img')
    wire=bytearray((templates/'payload.v1').read_bytes());wire[80:112]=bytes.fromhex(digest(templates/'root.img'))
    wire[224:]=hashlib.sha256(wire[:224]).digest();(templates/'payload.v1').write_bytes(wire)
    manifest=json.loads((templates/'manifest.json').read_text());manifest['roles']['root.img']['sha256']=digest(templates/'root.img')
    manifest['factory_packages']=bundle;manifest['factory_seed_host']=str(tool_out)
    (templates/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    (out/'manifest.json').write_text(json.dumps({'result':'PASS','packages':bundle,'host':str(tool_out),
        'templates':str(templates),'scope':'verified unsigned factory artifacts under installer authority; ordinary runtime signatures unchanged'},indent=2)+'\n')
    return manifest
