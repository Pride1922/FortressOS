#!/usr/bin/env python3
"""Actual detached provisioning; disposable regular EXT4 images and Linux audit."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import test_root_phase0 as fixture

ROOT=Path(__file__).resolve().parents[1]


def main():
    parent=ROOT/'build/root-phase4';parent.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='provision-',dir=parent))
    manifest={'scope':'actual detached EXT4/account provisioning, host transport only','result':'FAIL','commands':[],
              'sources':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for base in ('src','tests','scripts','user')
                         for p in (ROOT/base).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.inc','.py','.asm')}}
    try:
        def run(cmd):
            manifest['commands'].append([str(x) for x in cmd]);return fixture.run(cmd,out/'audit.log',300)
        tree=out/'tree';tree.mkdir()
        for name in ('etc','var','home','home/operator','root'):(tree/name).mkdir(parents=True,exist_ok=True)
        (tree/'etc/passwd').write_text('root:x:0:0::/root:/bin/shell\noperator:x:1000:1000::/home/operator:/bin/shell\n')
        (tree/'etc/group').write_text('root:x:0:\noperator:x:1000:\nwheel:x:10:operator\n')
        (tree/'etc/shadow').write_text('root:!:::::::\noperator:!:::::::\n')
        binary=out/'host'
        run(['gcc','-std=c11','-O1','-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-no-pie','-pthread',
             '-Itests/ext4_host','-Itests/host','-Isrc/include','-Isrc/fs','-Isrc/drivers','-Isrc/mm','-Isrc/kernel','-Isrc/arch/x86_64',
             'tests/install_provision_host.c','tests/ext4_fault_disk.c','src/lib/account_redo.c','user/tools/userdb.c','user/tools/digest.c','-o',binary])
        image=out/'template.img'
        with image.open('wb') as f:f.truncate(256*1024*1024)
        run(['mke2fs','-q','-t','ext4','-b','4096','-I','256','-O',fixture.FEATURES,'-E','lazy_itable_init=0',
             '-m','0','-g','4096','-N','4096','-J','size=4','-d',tree,image])
        commands=['set_inode_field / uid 0','set_inode_field / gid 0','set_inode_field / mode 040755']
        for path in tree.rglob('*'):
            name='/'+path.relative_to(tree).as_posix();home=name=='/home/operator'
            mode=(0o40700 if name in ('/root','/home/operator') else 0o40755) if path.is_dir() else (0o100600 if name=='/etc/shadow' else 0o100644)
            commands.extend([f'set_inode_field {name} uid {1000 if home else 0}',f'set_inode_field {name} gid {1000 if home else 0}',f'set_inode_field {name} mode 0{mode:o}'])
        attrs=out/'attrs';attrs.write_text('\n'.join(commands)+'\n')
        run(['debugfs','-w','-f',attrs,image]);fixture.initialize_journal(image,out)
        for sector in (512,4096):
            disk=out/('root-'+str(sector)+'.img');run(['cp','--sparse=always',image,disk]);run([binary,disk,str(sector)]);run(['e2fsck','-fn',disk])
            for name in ('passwd','group','shadow'):
                extracted=out/(str(sector)+'-'+name);run(['debugfs','-R',f'dump /etc/{name} {extracted}',disk])
                if name!='shadow':assert extracted.read_bytes()==(tree/'etc'/name).read_bytes()
                else:assert extracted.read_bytes().startswith(b'root:!:::::::\noperator:$5$rounds=100000$')
        assert all(hashlib.sha256((ROOT/p).read_bytes()).hexdigest()==h for p,h in manifest['sources'].items())
        manifest['result']='PASS';print('PASS detached provisioning and independent Linux EXT4 audit (512/4096 sectors)');print(out)
    finally:(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')


if __name__=='__main__':main()
