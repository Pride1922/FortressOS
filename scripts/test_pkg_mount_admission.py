"""Actual copied-source account service/redo and JBD2 paired-state gate.

Public test enrollment keys, disposable regular-file fixtures and RAM sector
fault adapter only. Never uses a native .origin witness or release build target.
"""
import argparse
import datetime
import hashlib
import json
import pathlib
import re
import shutil
import struct
import os
import subprocess
from concurrent.futures import ThreadPoolExecutor
from test_ext4_time_host import fixture
from test_pkg_enrollment import signed, policy
from test_pkg_service_integration import function
from pkg_account_service_overlay import apply
from pkg_mount_overlay import apply_mount
from pkg_mount_vectors import vectors

ROOT = pathlib.Path(__file__).resolve().parents[1]


def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--block', type=int, choices=(1024, 2048, 4096))
    parser.add_argument('--smoke', action='store_true')
    parser.add_argument('--low-space', action='store_true', help='exact effective account capacity behind durable package escrow')
    parser.add_argument('--physical-low-space', action='store_true', help='fill disposable filesystem to exact block/inode capacity')
    parser.add_argument('--jobs', type=int, choices=range(1, 9), default=8)
    args = parser.parse_args()
    assert not (args.low_space and args.physical_low_space)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H%M%S-%f')
    out = ROOT/'.codex-remote-attachments'/('pkg-mount-admission-'+stamp)
    work = out/'workspace'; work.mkdir(parents=True)
    sources = {}
    for folder in ('src', 'tests', 'user', 'scripts'):
        for p in (ROOT/folder).rglob('*'):
            if p.is_file() and p.suffix in ('.c', '.h', '.inc', '.py', '.mk') and '__pycache__' not in p.parts:
                name = str(p.relative_to(ROOT)); sources[name] = digest(p)
                dest = work/name; dest.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(p, dest)
    guards = {str(p.relative_to(ROOT)): digest(p) for p in
              [ROOT/'AGENTS.md', ROOT/'README.md', ROOT/'assets/ssl/ca-certificates.sha256']+
              [p for p in (ROOT/'bin').iterdir() if p.is_file() and p.name in
               ('fortress.elf', 'initramfs.tar', 'fortress.iso', 'fortress.img')]}
    manifest = {'status': 'INCOMPLETE', 'sources': sources, 'guards_before': guards,
                'commands': [], 'overlay': [], 'cases': [],
                'scope': 'Private authenticated package state/trust/grant mount-preview and account integration; no production or boot acceptance'}

    def replace(name, before, after):
        p = work/name; text = p.read_text()
        if text.count(before) != 1:
            manifest.update(status='FAIL', error=repr((name,before,text.count(before))))
            (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
            raise AssertionError((name,before))
        p.write_text(text.replace(before, after))
        manifest['overlay'].append({'path': name, 'before': before, 'after': after})

    replace('src/fs/ext4_transaction.inc', 'static int e4_semantic_commit(ext4_engine_t *e) {',
            'static int ad_core_commit(ext4_engine_t *e) {')
    p = 'src/fs/ext4_orphan.inc'
    text = (work/p).read_text()
    old = function(text, 'int ext4_engine_orphan_unlink(ext4_engine_t *e,uint32_t parent,const char *name,bool directory) {')
    before = '    r=e4_orphan_add(e,ino,raw);if (r) return e4_namespace_end(e,r);'
    assert old.count(before) == 1
    replace(p, old, old.replace(before, '    r=ad_activate(e,ino,parent);if(r)return e4_namespace_end(e,r);\n'+before))
    before = '        e4_inode_sum(e->fs,ino,raw);r=e4_namespace_end(e,0);if (r) return r;'
    replace(p, before, '        if(deleted){r=ad_retire(e,ino);if(r)return e4_namespace_end(e,r);}\n'+before)
    replace('src/fs/ext4_mount_journal.inc', '    e4_engine_dispose(engine);engine=NULL;',
            '    e4_mount_stage(stage,"account-provenance-preview");\n    r=ad_preview(engine);if(r)goto fail;\n    e4_engine_dispose(engine);engine=NULL;')
    apply(work, replace)
    apply_mount(work, replace)
    generated = work/'tests/account-service-durable.c'
    generated.write_text('#define _POSIX_C_SOURCE 200809L\n#include "account_redo.h"\nstruct ext4_engine;struct ext4_mount;struct vfs_node;\nstatic struct ext4_mount *sv_fs;\nstatic unsigned sv_role=ACCOUNT_ROLE_COUNT;\nstatic void teardown(void);\nstatic int e4_semantic_commit(struct ext4_engine *);\nstatic int e4_namespace_end(struct ext4_engine *,int);\nstatic int ad_activate(struct ext4_engine *,unsigned,unsigned);\nstatic int ad_retire(struct ext4_engine *,unsigned);\nstatic int ad_preview(struct ext4_engine *);\nstatic int ad_validate(struct ext4_engine *);\nstatic int sv_state_extra(void);\nstatic const char *sv_path(unsigned);\nstatic void sv_seed(struct ext4_mount *);\nstatic void sv_seed_state(struct ext4_mount *);\nstatic bool sv_pin_deny(struct vfs_node *);\nstatic int sv_boot_bind(void *,account_io_t *);\nstatic int sv_boot_end(void *);\nstatic void sv_relax(void);\nstatic int sv_begin(void *);\nstatic int sv_end(void *);\nstatic int sv_issue_pre(void *,uint64_t);\nstatic int sv_issue(void *,account_workspace_t *,uint64_t);\nstatic int sv_complete(void *);\n#define TEST_PERMISSIONS_ENFORCEMENT\n#define main account_previous_main\n#include "ext4_mount_host.c"\n#undef main\n#include "pkg_account_durable.inc"\n#include "src/kernel/accounts.c"\nuint64_t *vmm_get_active_pml4_virt(void){return (void *)1;}\nbool vmm_validate_user_range(uint64_t *p,uintptr_t a,size_t n,bool output){\n (void)p;(void)output;return a>=4096&&a<0x800000000000ull&&n<=0x800000000000ull-a;\n}\n' + function((work/'src/kernel/syscall.c').read_text(),'int64_t syscall_from_vfs_error(') + '\n#include "pkg_account_service_durable.inc"\n')

    prefix='struct ext4_engine;struct ext4_mount;struct vfs_node;'
    declarations='\nstatic bool ng_pause;\nstatic int ng_headroom(struct ext4_mount *);\nstatic int ng_gate(struct ext4_engine *,bool);\nstatic int ng_stamp(struct ext4_engine *);\nstatic int ng_extra(void);\nstatic void ng_preview(struct ext4_engine *);\nstatic void ng_seed_files(struct ext4_mount *);\nstatic void ng_seed_records(struct ext4_mount *);\nstatic void ng_policy_load(const uint8_t *);\nstatic int ng_fence(struct ext4_engine *);\nstatic void ng_guard_tests(struct ext4_mount *);\n'
    generated.write_text(generated.read_text().replace(prefix,prefix+declarations)+'\n#include "pkg_mount_admission.inc"\n')
    env=dict(os.environ)
    inputs=out/'fixture-inputs';extra=vectors(inputs);env['PKG_MOUNT_INPUT']=str(inputs)

    def run(command, label, timeout=180):
        manifest['commands'].append(command)
        log_path=out/(label+'.log')
        with log_path.open('w') as log:
            result = subprocess.run(command, cwd=work, stdout=log, stderr=subprocess.STDOUT, text=True, timeout=timeout, env=dict(env))
        text=log_path.read_text()
        assert result.returncode == 0, (label, result.returncode, text[-2000:])
        return text

    manifest['copied_inputs'] = {p: digest(work/p) for p in sources}

    def audit(img,label):
        before=digest(img);run(['e2fsck','-fn',str(img)],label+'-fsck');records={}
        for name in ('grant.v1','ownership.v1','state.v1','trust.v1','account-generation.v1','origin.v1'):
            dump=out/(label+'-'+name+'.dump');run(['debugfs','-R',f'dump /pkg/{name} {dump}',str(img)],label+'-'+name);records[name]=dump.read_bytes()
        l,t,a,slot=records['grant.v1'],records['ownership.v1'],records['account-generation.v1'],records['origin.v1']
        for b in (l,a,slot,records['state.v1'],records['trust.v1']):assert len(b)==1024 and b[992:]==hashlib.sha256(b[:992]).digest()
        assert len(t)==8192 and t[8160:]==hashlib.sha256(t[:8160]).digest() and l[264:296]==hashlib.sha256(t).digest()
        assert a[112:144]==hashlib.sha256(slot).digest() and a[448:480]==hashlib.sha256(l).digest() and a[480:512]==hashlib.sha256(records['trust.v1']).digest() and a[512:544]==hashlib.sha256(records['state.v1']).digest()
        assert struct.unpack_from('<Q',a,440)[0]==struct.unpack_from('<Q',l,24)[0]==struct.unpack_from('<Q',t,48)[0]
        assert struct.unpack_from('<H',a,20)[0]==3 and a[22:24]==slot[22:24]==bytes(2)
        for i in range(2):assert struct.unpack_from('<Q',l,296+8*i)[0]==struct.unpack_from('<Q',l,312+8*i)[0]+struct.unpack_from('<Q',l,328+8*i)[0]
        bs=int(label.split('-')[0]);phase=struct.unpack_from('<H',l,22)[0];charged=[0,0]
        for i in range(struct.unpack_from('<H',t,22)[0]):
            row=t[256+64*i:320+64*i];role=struct.unpack_from('<H',row)[0];ino,gen=struct.unpack_from('<II',row,4)
            text=run(['debugfs','-R',f'stat <{ino}>',str(img)],label+f'-inode-{role}')
            assert int(re.search(r'Inode:\s+(\d+)',text)[1])==ino and int(re.search(r'Generation:\s+(\d+)',text)[1])==gen
            assert re.search(r'User:\s+0\s+Group:\s+0',text) and int(re.search(r'Links:\s+(\d+)',text)[1])==1
            assert int(re.search(r'Mode:\s+([0-7]+)',text)[1],8)==(0o755 if role==17 else 0o600)
            assert int(re.search(r'Size:\s+(\d+)',text)[1])==struct.unpack_from('<Q',row,32)[0]
            actual=int(re.search(r'Blockcount:\s+(\d+)',text)[1]);assert actual*512==struct.unpack_from('<Q',row,40)[0]*bs
            charged[0]+=actual*512//bs;charged[1]+=1
        assert charged==list(struct.unpack_from('<QQ',l,328))
        if phase>=2:
            pending=out/(label+'-pending.dump');path='/pkg/transaction.v1/'+('retired.v1' if phase==3 else 'pending.v1');run(['debugfs','-R',f'dump {path} {pending}',str(img)],label+'-pending')
            assert a[544:576]==l[136:168]==hashlib.sha256(pending.read_bytes()).digest()
        assert records['state.v1'] in ((inputs/'old.bin').read_bytes(),(inputs/'next.bin').read_bytes()) and records['trust.v1']==(inputs/'trust.bin').read_bytes()
        assert digest(img)==before

    try:
        flags = ['gcc', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined', '-fstack-usage',
                 '-Wall', '-Wextra', '-Werror', '-no-pie', '-pthread', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections']
        flags += ['-I'+p for p in ('tests', 'tests/ext4_host', 'tests/host', 'src/include', 'src/fs',
                                   'src/drivers', 'src/kernel', 'src/arch/x86_64', 'src/mm', 'user/tools', 'src/lib/crypto/upstream/src', '.')]
        objects = []
        for tag, path in [('mono', 'src/lib/crypto/upstream/src/monocypher.c'),
                          ('ed', 'src/lib/crypto/upstream/src/optional/monocypher-ed25519.c')]:
            target = out/(tag+'.o'); objects.append(str(target))
            run(flags+['-include', 'src/lib/crypto/namespace.h', '-c', path, '-o', str(target)], 'compile-'+tag)
        units = ['tests/account-service-durable.c', 'tests/pkg_enrollment_candidate.c', 'tests/ext4_fault_disk.c',
                 'src/fs/permission_values.c', 'src/kernel/creds.c', 'user/tools/digest.c',
                 'src/lib/crypto/hash.c', 'src/lib/crypto/curve.c', 'src/lib/account_redo.c', 'user/tools/userdb.c', 'tests/pkg_mount_candidate.c', 'tests/pkg_grant_candidate.c', 'user/pkg/catalog.c', 'user/pkg/verify.c', 'user/pkg/format.c', 'user/pkg/manifest.c', 'user/tools/gzip.c']
        dep = run(flags+['-MM', '-MT', 'probe', *units], 'dependencies')
        names = {p for p in dep.replace('\\\n', ' ').split() if not p.endswith(':')}
        names.update(('src/lib/crypto/upstream/src/monocypher.c','src/lib/crypto/upstream/src/optional/monocypher-ed25519.c'))
        manifest['compiled_inputs'] = {p: digest(work/p) for p in sorted(names)}
        run(flags+units+objects+['-o', str(out/'probe')], 'compile')
        run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-mno-red-zone','-mno-sse','-mno-mmx','-msoft-float','-fstack-usage','-Wframe-larger-than=1024','-I.','-Isrc/include','-Isrc/kernel','-Iuser/tools','-c','tests/pkg_mount_candidate.c','-o',str(out/'mount-frame.o')],'compile-freestanding')
        for bs in ((args.block,) if args.block else (1024,2048,4096)):
            image=fixture(out,bs,inodes=128,size_mib=32 if bs==4096 else 16);before=digest(image)
            for ss in (512,4096):
                for phase,publication in ((0,0),(0,2),(1,0),(2,0),(2,1),(2,2),(3,2)):
                    env['PKG_MOUNT_PHASE']=str(phase);env['PKG_MOUNT_PUBLICATION']=str(publication)
                    tag=f'{bs}-{ss}-p{phase}-v{publication}';pre=out/tag
                    run([str(out/'probe'),'setup',str(image),str(ss),str(pre)],tag+'-setup')
                    setup=pathlib.Path(str(pre)+'-setup.img');body=pathlib.Path(str(pre)+'-body.img').read_bytes();ids=[body[40+16*i:56+16*i] for i in range(5)]
                    en=out/(tag+'-enrollment.bin');desc=signed(body);en.write_bytes(desc);pol=out/(tag+'-policy.bin');pol.write_bytes(policy(desc,uuids=ids)+extra)
                    if args.physical_low_space:
                        data=setup.read_bytes();la=struct.unpack_from('<Q',body,416+128+48)[0]*bs
                        d=(8192+bs-1)//bs;need=(9*(d+(d>4))+2-3,8)
                        g=struct.unpack_from('<QQ',data,la+312);target=(g[0]+need[0],g[1]+need[1])
                        empty=out/'empty';empty.write_bytes(b'')
                        count=struct.unpack_from('<I',data,1040)[0]-target[1]
                        # One final inode belongs to the dense filler file.
                        commands=out/(tag+'-pressure.commands')
                        commands.write_text(''.join(f'write {empty} /pressure{i}\n' for i in range(count-1)))
                        run(['debugfs','-w','-f',str(commands),str(setup)],tag+'-pressure-inodes')
                        filler=out/(tag+'-filler');adjust=0
                        for attempt in range(4):
                            free=struct.unpack_from('<I',setup.read_bytes(),1036)[0]
                            n=free-target[0]+adjust;assert n>0
                            filler.write_bytes(b'P'*(n*bs))
                            run(['debugfs','-w','-R',f'write {filler} /pressure-bytes',str(setup)],tag+f'-pressure-blocks-{attempt}')
                            actual=struct.unpack_from('<II',setup.read_bytes(),1036)
                            if actual==target:break
                            adjust=actual[0]-target[0]
                            run(['debugfs','-w','-R','rm /pressure-bytes',str(setup)],tag+f'-pressure-retry-{attempt}')
                        else:raise AssertionError(('physical pressure',actual,target))
                        filler.unlink();run(['e2fsck','-fn',str(setup)],tag+'-pressure-fsck')
                    if args.low_space:
                        # Change only fixture grant balances, then bind the exact
                        # new ledger in the account snapshot. Actual F is read
                        # from the audited filesystem, never fabricated.
                        raw=bytearray(setup.read_bytes())
                        la=struct.unpack_from('<Q',body,416+128+48)[0]*bs
                        aa=struct.unpack_from('<Q',body,416+5*128+48)[0]*bs
                        ledger=bytearray(raw[la:la+1024]);account=bytearray(raw[aa:aa+1024])
                        d=(8192+bs-1)//bs;remaining=(9*(d+(d>4))+2-3,8)
                        # IDLE cannot retain escrow by grammar; exercise active
                        # grants here, with ordinary IDLE regression unchanged.
                        if phase:
                            for dim in range(2):
                                f=struct.unpack_from('<I',raw,1036+4*dim)[0]
                                g=f-remaining[dim];q=struct.unpack_from('<Q',ledger,328+8*dim)[0]
                                struct.pack_into('<Q',ledger,312+8*dim,g)
                                struct.pack_into('<Q',ledger,296+8*dim,g+q)
                            ledger[992:]=hashlib.sha256(ledger[:992]).digest()
                            account[448:480]=hashlib.sha256(ledger).digest();account[992:]=hashlib.sha256(account[:992]).digest()
                            raw[la:la+1024]=ledger;raw[aa:aa+1024]=account;setup.write_bytes(raw)
                    def probe(mode,img,prefix,rules=pol):return run([str(out/'probe'),mode,str(img),str(ss),str(en),str(rules),str(prefix)],prefix.name)
                    normal=out/(tag+'-normal');text=probe('normal',setup,normal);assert 'PASS family=1 auth=2' in text
                    commits=int(re.search(r'PASS update commits=(\d+)',text)[1]);assert commits>=29
                    final=pathlib.Path(str(normal)+'-final.img');audit(final,normal.name)
                    again=out/(tag+'-again');text=probe('recover',final,again);assert 'commits=0' in text
                    case={'block':bs,'sector':ss,'phase':phase,'publication':publication,'update_commits':commits,'cuts':[]}
                    if not args.smoke:
                        def cancel(cut):
                            prefix=out/(tag+f'-cut{cut}');report=probe(f'update={cut}',setup,prefix);broken=pathlib.Path(str(prefix)+'-interrupted.img');reports=[]
                            for restart in range(2):
                                dest=out/(prefix.name+f'-restart{restart}');reports.append(probe('recover',broken,dest));new=pathlib.Path(str(dest)+'-final.img');audit(new,dest.name)
                                if restart:assert 'commits=0' in reports[-1]
                                broken.unlink();broken=new
                            broken.unlink();return {'cut':cut,'interrupt':report,'recoveries':reports}
                        with ThreadPoolExecutor(max_workers=args.jobs) as pool:case['cuts']=list(pool.map(cancel,(1,13,29,commits)))
                    pause=out/(tag+'-pause');probe('tick-pause',setup,pause);journal=pathlib.Path(str(pause)+'-interrupted.img')
                    recovered=out/(tag+'-journal');text=probe('recover',journal,recovered);assert 'grant=2 newer=1' in text
                    audit(pathlib.Path(str(recovered)+'-final.img'),recovered.name)
                    if not args.smoke and phase==2 and publication==0:
                        baseline=out/(tag+'-tick-baseline');baseline_text=probe('tick',setup,baseline);events=int(re.search(r'PASS tick events=(\d+)',baseline_text)[1]);case['grant_tick_events']=events
                        def io_cut(pair):
                            profile,cut=pair;prefix=out/(tag+f'-io-{profile}-{cut}');probe(f'tick={profile}:{cut}',setup,prefix);broken=pathlib.Path(str(prefix)+'-interrupted.img');reports=[]
                            for restart in range(2):
                                dest=out/(prefix.name+f'-restart{restart}');reports.append(probe('recover',broken,dest));new=pathlib.Path(str(dest)+'-final.img');audit(new,dest.name)
                                if restart:assert 'commits=0' in reports[-1]
                                broken.unlink();broken=new
                            broken.unlink();return {'profile':profile,'cut':cut,'recoveries':reports}
                        with ThreadPoolExecutor(max_workers=args.jobs) as pool:case['grant_tick_io_cuts']=list(pool.map(io_cut,[(p,c) for p in range(8) for c in sorted({0,events//2,events-1})]))
                    negatives=[]
                    def reject(label,data,rules=pol):
                        bad=out/(tag+'-bad-'+label+'.img');bad.write_bytes(data);before=digest(bad);dest=out/(tag+'-reject-'+label)
                        report=probe('reject',bad,dest,rules);assert 'PASS reject writes=0 flushes=0' in report and digest(bad)==before;bad.unlink();negatives.append(label)
                    raw=setup.read_bytes()
                    assert struct.unpack_from('<H',body,416+2*128+32)[0]==1 and struct.unpack_from('<I',body,416+2*128+44)[0]==8192//bs
                    # Independent external policy cannot be adopted from target bytes.
                    for label,at in [('root-version',400),('catalog-floor',408),('catalog-hash',424),('unknown-key',464),('base-version',752),('platform',720),('clock',416)]:
                        rules=bytearray(pol.read_bytes())
                        if label=='clock':rules[at:at+8]=bytes(8)
                        else:rules[at]^=1
                        wrong=out/(tag+'-'+label+'.policy');wrong.write_bytes(rules);reject(label,raw,wrong)
                    def location(index):return struct.unpack_from('<Q',body,416+index*128+48)[0]*bs
                    def patch(label,kind):
                        data=bytearray(raw);la,ta,aa=location(1),location(2),location(5);l=bytearray(data[la:la+1024]);t=bytearray(data[ta:ta+8192]);a=bytearray(data[aa:aa+1024])
                        if kind=='snapshot':struct.pack_into('<Q',a,440,2)
                        elif kind=='free':
                            struct.pack_into('<Q',l,312,2**32);struct.pack_into('<Q',l,296,2**32+struct.unpack_from('<Q',l,328)[0])
                        elif kind.startswith('idle-state-'):
                            sa=location(6);state=bytearray(data[sa:sa+1024])
                            if kind=='idle-state-command':state[193:197]=b'evil'
                            else:state[120]^=1
                            state[992:]=hashlib.sha256(state[:992]).digest();data[sa:sa+1024]=state;state_hash=hashlib.sha256(state).digest();l[168:200]=l[200:232]=state_hash;a[512:544]=state_hash
                        elif kind=='operation':struct.pack_into('<H',l,348,2)
                        elif kind=='steps':struct.pack_into('<Q',l,424,1)
                        elif kind=='generation':struct.pack_into('<I',t,256+8,struct.unpack_from('<I',t,256+8)[0]+1)
                        elif kind=='orphan':struct.pack_into('<I',t,256+12,2)
                        elif kind=='map':
                            q=struct.unpack_from('<Q',t,256+40)[0]+1;struct.pack_into('<Q',t,256+40,q);struct.pack_into('<Q',l,328,struct.unpack_from('<Q',l,328)[0]+1);struct.pack_into('<Q',l,296,struct.unpack_from('<Q',l,296)[0]+1)
                        t[8160:]=hashlib.sha256(t[:8160]).digest();l[264:296]=hashlib.sha256(t).digest();l[992:]=hashlib.sha256(l[:992]).digest()
                        if kind!='snapshot':a[448:480]=hashlib.sha256(l).digest()
                        a[992:]=hashlib.sha256(a[:992]).digest();data[la:la+1024]=l;data[ta:ta+8192]=t;data[aa:aa+1024]=a;reject(label,data)
                    patch('account-grant-snapshot','snapshot')
                    if (args.low_space or args.physical_low_space) and phase:
                        for dim in range(2):
                            data=bytearray(raw);la,aa=location(1),location(5)
                            l=bytearray(data[la:la+1024]);a=bytearray(data[aa:aa+1024])
                            for at in (296+8*dim,312+8*dim):struct.pack_into('<Q',l,at,struct.unpack_from('<Q',l,at)[0]+1)
                            l[992:]=hashlib.sha256(l[:992]).digest();a[448:480]=hashlib.sha256(l).digest();a[992:]=hashlib.sha256(a[:992]).digest()
                            data[la:la+1024]=l;data[aa:aa+1024]=a;reject('capacity-minus-one-'+str(dim),data)
                    if phase==0 and publication==2:
                        for kind in ('idle-state-command','idle-state-artifact'):patch(kind,kind)
                    if phase:
                        for kind in ('free','generation','orphan','map','operation','steps'):patch(kind,kind)
                    # Alter exact pending semantics while preserving every outer seal.
                    if phase>=2:
                        pending_path='/pkg/transaction.v1/'+('retired.v1' if phase==3 else 'pending.v1')
                        blocks=run(['debugfs','-R','blocks '+pending_path,str(setup)],tag+'-pending-map');at=int(re.search(r'(?m)^([0-9]+)',blocks)[1])*bs
                        for label,field in [('pending-generation',240),('pending-transaction',24),('pending-trust',248),('pending-state',104),('pending-time',264)]:
                            data=bytearray(raw);rec=bytearray(data[at:at+1024]);rec[field]^=1;rec[992:]=hashlib.sha256(rec[:992]).digest();data[at:at+1024]=rec
                            la,aa=location(1),location(5);l=bytearray(data[la:la+1024]);l[136:168]=hashlib.sha256(rec).digest();l[992:]=hashlib.sha256(l[:992]).digest();data[la:la+1024]=l
                            a=bytearray(data[aa:aa+1024]);a[448:480]=hashlib.sha256(l).digest();a[544:576]=hashlib.sha256(rec).digest();a[992:]=hashlib.sha256(a[:992]).digest();data[aa:aa+1024]=a;reject(label,data)
                    # Valid committed/uncheckpointed journal, wrong independent policy.
                    reject('journal-policy',journal.read_bytes(),out/(tag+'-catalog-floor.policy'))
                    expired=bytearray(pol.read_bytes());struct.pack_into('<Q',expired,416,1792540800+120);rules=out/(tag+'-expired.policy');rules.write_bytes(expired)
                    if phase==1:reject('expired-new-admission',raw,rules)
                    else:
                        old=out/(tag+'-expired-existing');report=probe('recover',setup,old,rules);assert 'PASS recovery' in report;pathlib.Path(str(old)+'-final.img').unlink()
                    case['zero_io_rejections']=negatives
                    case['status']='PASS';manifest['cases'].append(case);print(tag,'PASS',flush=True)
                bad_inputs=out/'forged-catalog-inputs';bad_extra=vectors(bad_inputs,bad_signature=True);env['PKG_MOUNT_INPUT']=str(bad_inputs);env['PKG_MOUNT_PHASE']='2';env['PKG_MOUNT_PUBLICATION']='0'
                tag=f'{bs}-{ss}-forged-catalog';prefix=out/tag
                run([str(out/'probe'),'setup',str(image),str(ss),str(prefix)],tag+'-setup')
                b=pathlib.Path(str(prefix)+'-body.img').read_bytes();desc=signed(b);en=out/(tag+'-enrollment.bin');en.write_bytes(desc);pol=out/(tag+'-policy.bin');pol.write_bytes(policy(desc,uuids=[b[40+16*i:56+16*i] for i in range(5)])+bad_extra)
                text=run([str(out/'probe'),'reject',str(prefix)+'-setup.img',str(ss),str(en),str(pol),str(out/(tag+'-reject'))],tag+'-reject');assert 'PASS reject writes=0 flushes=0' in text
                manifest.setdefault('forged_catalog_cases',[]).append(tag)
                unsupported=out/'unsupported-inputs';unsupported_extra=vectors(unsupported,with_doc=True);env['PKG_MOUNT_INPUT']=str(unsupported)
                tag=f'{bs}-{ss}-unsupported';prefix=out/tag;run([str(out/'probe'),'setup',str(image),str(ss),str(prefix)],tag+'-setup')
                b=pathlib.Path(str(prefix)+'-body.img').read_bytes();desc=signed(b);en=out/(tag+'-enrollment.bin');en.write_bytes(desc);pol=out/(tag+'-policy.bin');pol.write_bytes(policy(desc,uuids=[b[40+16*i:56+16*i] for i in range(5)])+unsupported_extra)
                text=run([str(out/'probe'),'reject',str(prefix)+'-setup.img',str(ss),str(en),str(pol),str(out/(tag+'-reject'))],tag+'-reject');assert 'PASS reject writes=0 flushes=0' in text
                manifest.setdefault('unsupported_archive_cases',[]).append(tag)
                foreign=out/'foreign-inputs';foreign_extra=vectors(foreign,foreign_genesis=True);env['PKG_MOUNT_INPUT']=str(foreign);env['PKG_MOUNT_PUBLICATION']='2'
                tag=f'{bs}-{ss}-foreign-lineage';prefix=out/tag;run([str(out/'probe'),'setup',str(image),str(ss),str(prefix)],tag+'-setup')
                b=pathlib.Path(str(prefix)+'-body.img').read_bytes();desc=signed(b);en=out/(tag+'-enrollment.bin');en.write_bytes(desc);pol=out/(tag+'-policy.bin');pol.write_bytes(policy(desc,uuids=[b[40+16*i:56+16*i] for i in range(5)])+foreign_extra)
                text=run([str(out/'probe'),'reject',str(prefix)+'-setup.img',str(ss),str(en),str(pol),str(out/(tag+'-reject'))],tag+'-reject');assert 'PASS reject writes=0 flushes=0' in text
                manifest.setdefault('foreign_lineage_cases',[]).append(tag);env['PKG_MOUNT_INPUT']=str(inputs)
            assert digest(image)==before
        manifest['status'] = 'PASS'
    except Exception as exc:
        manifest['status'] = 'FAIL'; manifest['error'] = repr(exc); raise
    finally:
        manifest['changed_sources'] = [p for p, h in sources.items() if digest(ROOT/p) != h]
        manifest['changed_compiled_copies'] = [p for p, h in manifest.get('compiled_inputs', {}).items() if digest(work/p) != h]
        manifest['changed_compiled_repository_sources'] = [p for p in manifest.get('compiled_inputs',{}) if p in sources and digest(ROOT/p)!=sources[p]]
        manifest['changed_all_copies'] = [p for p, h in manifest['copied_inputs'].items() if digest(work/p) != h]
        manifest['guards_after'] = {p: digest(ROOT/p) for p in guards}
        manifest['preserved'] = not manifest['changed_compiled_repository_sources'] and not manifest['changed_compiled_copies'] and not manifest['changed_all_copies'] and guards == manifest['guards_after']
        if not manifest['preserved']: manifest['status'] = 'FAIL'
        manifest['artifacts'] = {p.name: digest(p) for p in out.iterdir() if p.is_file() and p.name != 'manifest.json'}
        (out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
        print('Evidence:', out, manifest['status'], flush=True)
    assert manifest['status'] == 'PASS'


if __name__ == '__main__':
    main()
