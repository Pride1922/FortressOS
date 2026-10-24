"""Disposable actual-code geometry gate before signed native enrollment."""
import argparse, datetime, hashlib, json, os, pathlib, shutil, struct, subprocess, tempfile
from pkg_enroll_fixture import create, gpt, command, inode, blocks, directory

ROOT=pathlib.Path(__file__).resolve().parents[1]
H=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--identities',action='store_true');args=parser.parse_args()
    out=ROOT/'.codex-remote-attachments'/(('pkg-enroll-identity-' if args.identities else 'pkg-enroll-geometry-')+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
    out.mkdir();work=out/'workspace';work.mkdir()
    m={'result':'INCOMPLETE','commands':[],'cases':[],
       'scope':'host structural read-only GPT/EXT4/JBD2/account-slot preview; no signed enrollment, mount publication, kernel boot or physical acceptance'}
    paths=[p for name in ('src','user','scripts','tests','docs') for p in (ROOT/name).rglob('*') if p.is_file() and '__pycache__' not in p.parts]
    paths += [ROOT/n for n in ('Makefile','PROTECTED.md','AGENTS.md')]
    paths += [p for p in (ROOT/'bin').iterdir() if p.is_file()]
    guards={str(p.relative_to(ROOT)):H(p) for p in paths};m['preservation_before']=guards
    def save():(out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n')
    def run(cmd,label,cwd=work):
        m['commands'].append(cmd);save()
        p=subprocess.run(cmd,cwd=cwd,capture_output=True,text=True,timeout=180)
        (out/(label+'.log')).write_text(p.stdout+p.stderr)
        if p.returncode:raise RuntimeError((label,p.returncode,p.stdout[-1800:],p.stderr[-1800:]))
        return p.stdout
    try:
        flags=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-DFORTRESS_DAC_ENFORCED','-fsanitize=address,undefined',
          '-fno-omit-frame-pointer','-no-pie','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-pthread','-Itests/ext4_host','-Itests/host',
          '-Isrc/include','-Isrc/fs','-Isrc/drivers','-Isrc/mm','-Isrc/lib']
        units=['tests/pkg_enroll_geometry_host.c','tests/ext4_fault_disk.c','src/fs/permission_values.c','src/kernel/creds.c','src/lib/account_redo.c']
        deps=run(flags+['-MM',*units],'dependencies',ROOT)
        names={os.path.normpath(p) for p in deps.replace('\\\n',' ').split() if not p.endswith(':')}
        for name in names:
            source=ROOT/name;assert source.resolve().is_relative_to(ROOT)
            dest=work/name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,dest)
        m['compiled_inputs']={name:H(work/name) for name in sorted(names)}
        binary=out/'geometry-host';run(flags+units+['-o',str(binary)],'compile')
        m['binary_sha256']=H(binary)
        # POSIX scratch avoids depending on DrvFS mode propagation.
        with tempfile.TemporaryDirectory(prefix='fortress-pkg-geometry-') as temp:
            temp=pathlib.Path(temp)
            base=temp/'base';base.mkdir();image,disk=create(base,identities=args.identities)
            raw=image.read_bytes();diskraw=disk.read_bytes()
            run(['e2fsck','-fn',str(image)],'linux-fsck-initial')
            for path in ('/etc','/var/lib/fortress/accounts',*('/pkg/'+n for n in ('enrollment.v1','ownership.v1','descriptor.v2'))):
                run(['debugfs','-R','stat '+path,str(image)],'linux-stat-'+path.replace('/','_'))
            variants=[('canonical',diskraw,'accept')]
            variants.append(('filesystem-read-error',diskraw,'refuse-io'))
            for label,path,parent,entries,slots in (
                ('etc-short-slot-capacity','/etc','/',[(inode(image,p),p.rsplit('/',1)[1]) for p in ('/etc/passwd','/etc/group','/etc/shadow')],5),
                ('private-short-slot-capacity','/var/lib/fortress/accounts','/var/lib/fortress',[],4)):
                bad=bytearray(raw)
                directory(bad,inode(image,path),inode(image,parent),entries,slots,blocks(image,path)[0])
                variants.append((label,bytes(gpt(bad)),'refuse'))
            normal=temp/'normal';normal.mkdir();_,nd=create(normal,False)
            variants.append(('unreserved-directories',nd.read_bytes(),'refuse'))
            # Metadata mutations are made through Linux debugfs, which reseals
            # the inode checksums; a checksum error cannot mask these checks.
            for label,cmd in (
                ('wrong-etc-mode','set_inode_field /etc mode 040700'),
                ('wrong-root-owner','set_inode_field / uid 1'),
                ('wrong-shadow-mode','set_inode_field /etc/shadow mode 0100644'),
                ('account-hardlink','set_inode_field /etc/passwd links_count 2'),
                ('wrong-private-owner','set_inode_field /var/lib/fortress/accounts uid 1000'),
                ('missing-shadow','unlink /etc/shadow')):
                p=temp/(label+'.img');p.write_bytes(raw)
                run(['debugfs','-w','-R',cmd,str(p)],'mutate-'+label)
                variants.append((label,bytes(gpt(p.read_bytes())),'refuse'))
            bad=bytearray(raw);physical=blocks(image,'/etc')[0];bad[physical*4096+4092]^=1
            variants.append(('directory-checksum',bytes(gpt(bad)),'refuse'))
            for label,offset in (('primary-gpt',512+16),('backup-gpt',len(diskraw)-512+16),
                                 ('primary-table',1024+16)):
                bad=bytearray(diskraw);bad[offset]^=1;variants.append((label,bytes(bad),'refuse'))
            # Structurally valid GPT with a different selected partition UUID.
            import pkg_enroll_fixture as fixture,uuid
            original=fixture.PART;fixture.PART=uuid.UUID('52345678-1234-4567-89ab-123456789abc')
            variants.append(('foreign-partition',bytes(gpt(raw)),'refuse'));fixture.PART=original
            for label,data,expected in variants:
                p=out/(label+'.img');p.write_bytes(data);before=H(p)
                result=run([str(binary),str(p),expected],label)
                assert H(p)==before
                assert data[69632*512:(69632+4096)*512]==diskraw[69632*512:(69632+4096)*512]
                m['cases'].append({'name':label,'expected':expected,'sha256':before,
                    'whole_image_unchanged':True,'sentinel_unchanged':True,'diagnostic':result.strip()})
            if args.identities:
                from pkg_enroll_identity import inventory
                witness,audit=inventory(image);wf=out/'identities.bin';wf.write_bytes(witness)
                (out/'identities.json').write_text(json.dumps(audit,indent=2)+'\n')
                canonical=out/'canonical.img';before=H(canonical)
                def identity_case(label,data,expected):
                    p=out/(label+'.witness');p.write_bytes(data);p_before=H(p)
                    result=run([str(binary),str(canonical),expected,str(p)],label)
                    assert H(p)==p_before and H(canonical)==before
                    m['cases'].append({'name':label,'expected':expected,'witness_sha256':p_before,
                        'whole_image_unchanged':True,'diagnostic':result.strip()})
                identity_case('all-20-identities',witness,'accept')
                for i in range(20):
                    for field,off in (('generation',8),('physical-map',48)):
                        bad=bytearray(witness);bad[32+128*i+off]^=1
                        identity_case(f'identity-{i+1}-{field}',bad,'refuse')
                for label,off in (('length',16),('version',20),('count',22),('reserved',24),
                                  ('role',32),('mode',44),('size',48),('extent-tail',136)):
                    bad=bytearray(witness);bad[off]^=1;identity_case('identity-header-'+label,bad,'refuse')
                m['identity_inventory']={'rows':20,'nonzero_generations':True,
                    'wire_sha256':H(wf),'authenticated':False}
            (out/'root.img').write_bytes(raw)
            for path in ('/etc/passwd','/etc/group','/etc/shadow'):
                from pkg_enroll_fixture import FILES
                dump=temp/(path.rsplit('/',1)[1]+'.bytes')
                run(['debugfs','-R',f'dump {path} {dump}',str(image)],'linux-bytes-'+path.rsplit('/',1)[1])
                assert dump.read_bytes()==FILES[path]
            run(['e2fsck','-fn',str(image)],'linux-fsck-final')
            shutil.copyfile(base/'generation.log',out/'generation.log')
            m['root_sha256']=H(out/'root.img')
        m['runtime_result']='PASS'
        m['boot_acceptance']='NOT RUN';m['signed_enrollment']='NOT IMPLEMENTED IN THIS STEP'
    except Exception as e:
        m['result']='FAIL';m['error']=repr(e);raise
    finally:
        changed=[name for name,digest in guards.items() if not (ROOT/name).is_file() or H(ROOT/name)!=digest]
        m['preservation_result']='FAIL' if changed else 'PASS';m['changed_inputs']=changed
        if m.get('runtime_result')=='PASS':m['result']='FAIL' if changed else 'PASS'
        save();print(m['result'],out,flush=True)

if __name__=='__main__':main()
