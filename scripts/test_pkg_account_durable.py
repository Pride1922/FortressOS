"""Actual copied-source JBD2 account generation/origin atomicity gate.

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
import subprocess
from concurrent.futures import ThreadPoolExecutor
from test_ext4_time_host import fixture
from test_pkg_enrollment import signed, policy
from test_pkg_service_integration import function

ROOT = pathlib.Path(__file__).resolve().parents[1]


def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def seal(b):
    b = bytearray(b); b[992:] = hashlib.sha256(b[:992]).digest(); return b


def pair_expected(body, sequence=5, generation=2):
    slot = bytearray(1024); slot[:16] = b'FORT-ACC-ORG-v1\0'
    struct.pack_into('<IHHQQQ', slot, 16, 1024, 1, 0, sequence, 5, generation)
    slot[48:64], slot[64:80], slot[80:112] = body[104:120], body[40:56], body[312:344]
    slot = seal(slot)
    state = bytearray(1024); state[:16] = b'FORT-ACC-GEN-v1\0'
    struct.pack_into('<IHHQQQ', state, 16, 1024, 1, 0, sequence, generation, 5)
    state[48:112] = slot[48:112]
    state[112:144] = hashlib.sha256(slot).digest()
    return seal(state), slot


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--block', type=int, choices=(1024, 2048, 4096))
    parser.add_argument('--jobs', type=int, choices=range(1, 9), default=8)
    args = parser.parse_args()
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H%M%S-%f')
    out = ROOT/'.codex-remote-attachments'/('pkg-account-durable-'+stamp)
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
                'scope': 'Private actual EXT4/JBD2 primitive, shadow role/pre-intent only; no account service, production admission or boot acceptance'}

    def replace(name, before, after):
        p = work/name; text = p.read_text(); assert text.count(before) == 1, (name, before)
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
    generated = work/'tests/account-durable.c'
    generated.write_text('struct ext4_engine;\nstatic int e4_semantic_commit(struct ext4_engine *);\n'
                         'static int ad_activate(struct ext4_engine *,unsigned,unsigned);\n'
                         'static int ad_retire(struct ext4_engine *,unsigned);\n'
                         'static int ad_preview(struct ext4_engine *);\n'
                         '#define TEST_PERMISSIONS_ENFORCEMENT\n'
                         '#define main account_previous_main\n#include "ext4_mount_host.c"\n#undef main\n'
                         '#include "pkg_account_durable.inc"\n')

    def run(command, label, timeout=180):
        manifest['commands'].append(command)
        log_path=out/(label+'.log')
        with log_path.open('w') as log:
            result = subprocess.run(command, cwd=work, stdout=log, stderr=subprocess.STDOUT, text=True, timeout=timeout)
        text=log_path.read_text()
        assert result.returncode == 0, (label, result.returncode, text[-2000:])
        return text

    manifest['copied_inputs'] = {p: digest(work/p) for p in sources}

    def audit(image, body, label, bs, sequence=5):
        before = digest(image)
        run(['e2fsck', '-fn', str(image)], label+'-fsck')
        for name, expected in [('origin.v1', pair_expected(body,sequence)[1]),
                               ('account-generation.v1', pair_expected(body,sequence)[0]),
                               ('enrollment.v1', bytes(512)), ('ownership.v1', bytes(8192))]:
            target = out/(label+'-'+name+'.dump')
            run(['debugfs', '-R', f'dump /pkg/{name} {target}', str(image)], label+'-'+name)
            assert target.read_bytes() == expected, (label, name)
        target = out/(label+'-unrelated.dump')
        run(['debugfs', '-R', f'dump /target.bin {target}', str(image)], label+'-unrelated')
        assert target.read_bytes() == b'O'*(3*bs)
        assert 'File not found' in run(['debugfs', '-R', 'stat /etc/shadow', str(image)], label+'-shadow')
        assert digest(image) == before

    try:
        flags = ['gcc', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined', '-fstack-usage',
                 '-Wall', '-Wextra', '-Werror', '-no-pie', '-pthread', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections']
        flags += ['-I'+p for p in ('tests', 'tests/ext4_host', 'tests/host', 'src/include', 'src/fs',
                                   'src/drivers', 'src/mm', 'user/tools', 'src/lib/crypto/upstream/src', '.')]
        objects = []
        for tag, path in [('mono', 'src/lib/crypto/upstream/src/monocypher.c'),
                          ('ed', 'src/lib/crypto/upstream/src/optional/monocypher-ed25519.c')]:
            target = out/(tag+'.o'); objects.append(str(target))
            run(flags+['-include', 'src/lib/crypto/namespace.h', '-c', path, '-o', str(target)], 'compile-'+tag)
        units = ['tests/account-durable.c', 'tests/pkg_enrollment_candidate.c', 'tests/ext4_fault_disk.c',
                 'src/fs/permission_values.c', 'src/kernel/creds.c', 'user/tools/digest.c',
                 'src/lib/crypto/hash.c', 'src/lib/crypto/curve.c']
        dep = run(flags+['-MM', '-MT', 'probe', *units], 'dependencies')
        names = {p for p in dep.replace('\\\n', ' ').split() if not p.endswith(':')}
        names.update(('src/lib/crypto/upstream/src/monocypher.c','src/lib/crypto/upstream/src/optional/monocypher-ed25519.c'))
        manifest['compiled_inputs'] = {p: digest(work/p) for p in sorted(names)}
        run(flags+units+objects+['-o', str(out/'probe')], 'compile')
        for bs in ((args.block,) if args.block else (1024, 2048, 4096)):
            image = fixture(out, bs, inodes=128, size_mib=32 if bs == 4096 else 16); image_before = digest(image)
            for ss in (512, 4096):
                tag = f'{bs}-{ss}'; prefix = out/tag
                run([str(out/'probe'), 'setup', str(image), str(ss), str(prefix)], tag+'-setup')
                setup = pathlib.Path(str(prefix)+'-setup.img'); body = pathlib.Path(str(prefix)+'-body.img').read_bytes()
                assert len(body) == 2464
                descriptor = signed(body); ids = [body[40+16*i:56+16*i] for i in range(5)]
                enrollment = out/(tag+'-enrollment.bin'); enrollment.write_bytes(descriptor)
                selector = out/(tag+'-policy.bin'); selector.write_bytes(policy(descriptor, uuids=ids))
                setup_before = digest(setup)
                def campaign(profile):
                    target = prefix if profile==0 else out/(tag+f'-profile-{profile}')
                    text = run([str(out/'probe'), 'campaign', str(setup), str(ss), str(enrollment), str(selector), str(target), str(profile)], tag+f'-campaign-{profile}', timeout=1800)
                    match = re.search(r'PASS durable cuts=(\d+) previews=(\d+) pairs=(\d+) partial=(\d+) newer=(\d+) protections=7', text)
                    assert match and int(match[5]) > 0
                    events = re.findall(r'AD op=(\d+) events=(\d+) cuts=(\d+)',text)
                    assert len(events)==5 and [int(x[0]) for x in events]==list(range(5))
                    assert sum(int(x[1]) for x in events)==int(match[1]) and all(x[1]==x[2] for x in events)
                    return {'profile':profile,'events':[int(x[1]) for x in events], 'counts':[int(match[i]) for i in range(1,6)]}
                with ThreadPoolExecutor(max_workers=args.jobs) as pool:
                    profiles=list(pool.map(campaign,range(8)))
                assert all(p['events']==profiles[0]['events'] for p in profiles)
                counts=[sum(p['counts'][i] for p in profiles) for i in range(5)]
                case = {'block': bs, 'sector': ss, 'cuts': counts[0], 'previews': counts[1],
                        'pairs': counts[2], 'partial': counts[3], 'newer_preview': counts[4], 'profiles':profiles,
                        'zero_io_protections':56,
                        'restarts_per_cut': 2, 'linux_audits': 0, 'negative_cases': 0}
                for suffix in ('normal', 'recovered'):
                    audit(pathlib.Path(str(prefix)+'-'+suffix+'.img'), body, tag+'-'+suffix, bs); case['linux_audits'] += 1
                for suffix in ('orphan', 'partial'):
                    original = pathlib.Path(str(prefix)+'-'+suffix+'.img'); unchanged = digest(original)
                    for repeat in range(2):
                        fresh = out/(tag+'-'+suffix+f'-fresh-{repeat}')
                        run([str(out/'probe'), 'recover', str(original), str(ss), str(enrollment), str(selector), str(fresh)], fresh.name)
                        result = pathlib.Path(str(fresh)+'-final.img'); audit(result, body, fresh.name, bs); case['linux_audits'] += 1
                        if not repeat:
                            assert digest(original) == unchanged
                        original = result
                data=bytearray(setup.read_bytes())
                slot_at=struct.unpack_from('<Q',body,416+128*4+48)[0]*bs
                state_at=struct.unpack_from('<Q',body,416+128*5+48)[0]*bs
                state,slot=pair_expected(body,2**64-6,1)
                data[slot_at:slot_at+1024],data[state_at:state_at+1024]=slot,state
                exhausted=out/(tag+'-exhaust-input.img');exhausted.write_bytes(data)
                end=out/(tag+'-exhaust')
                run([str(out/'probe'),'exhaust',str(exhausted),str(ss),str(enrollment),str(selector),str(end)],tag+'-exhaust')
                audit(pathlib.Path(str(end)+'-final.img'),body,tag+'-exhaust-final',bs,2**64-2)
                case['linux_audits']+=1;case['zero_io_protections']+=1
                # Negative contents retain real inode/map ownership. Re-seal both
                # records where needed so actual preflight tests semantic binding.
                pristine = pathlib.Path(str(prefix)+'-orphan.img').read_bytes()
                slot_at = struct.unpack_from('<Q', body, 416+128*4+48)[0]*bs
                state_at = struct.unpack_from('<Q', body, 416+128*5+48)[0]*bs
                variants = [('missing-origin', 0), ('slot-generation', 1), ('slot-sequence', 2),
                            ('wrong-role', 3), ('wrong-inode', 4), ('wrong-parent', 5),
                            ('wrong-map', 6), ('active-with-idle', 7), ('generation-overflow', 8),
                            ('sequence-overflow', 9), ('state-reserved', 10), ('empty-with-orphan', 11),
                            ('sequence-near-overflow', 12), ('wrong-actual-map', 13), ('marker-digest', 14)]
                for label, kind in variants:
                    data = bytearray(pristine); slot = bytearray(data[slot_at:slot_at+1024]); state = bytearray(data[state_at:state_at+1024])
                    if kind == 0: slot = bytearray(1024)
                    elif kind == 1: struct.pack_into('<Q', slot, 40, 3)
                    elif kind == 2: struct.pack_into('<Q', slot, 24, 2)
                    elif kind == 3: struct.pack_into('<H', slot, 112, 1)
                    elif kind == 4: struct.pack_into('<I', slot, 116, 11)
                    elif kind == 5: struct.pack_into('<I', slot, 124, 11)
                    elif kind == 6: struct.pack_into('<Q', slot, 168, 1)
                    elif kind == 7: struct.pack_into('<H', state, 22, 0)
                    elif kind == 8:
                        struct.pack_into('<Q', state, 32, 2**64-1); struct.pack_into('<Q', slot, 40, 2**64-1)
                    elif kind == 9:
                        struct.pack_into('<Q', state, 24, 2**64-1); struct.pack_into('<Q', slot, 24, 2**64-1)
                    elif kind == 10: state[144] = 1
                    elif kind == 11:
                        struct.pack_into('<H', slot, 22, 0); slot[112:992] = bytes(880)
                    elif kind == 12:
                        struct.pack_into('<Q', state, 24, 2**64-2); struct.pack_into('<Q', slot, 24, 2**64-2)
                    elif kind == 13:
                        struct.pack_into('<Q', slot, 168, 7000); struct.pack_into('<Q', slot, 416, 7000)
                    else:
                        marker_at=struct.unpack_from('<Q',body,416+48)[0]*bs; data[marker_at]^=1
                    slot = seal(slot); state[112:144] = hashlib.sha256(slot).digest(); state = seal(state)
                    data[slot_at:slot_at+1024], data[state_at:state_at+1024] = slot, state
                    rejected = out/(tag+'-reject-'+label+'.img'); rejected.write_bytes(data); before = digest(rejected)
                    run([str(out/'probe'), 'reject', str(rejected), str(ss), str(enrollment), str(selector), str(out/(tag+'-reject-'+label))], tag+'-reject-'+label)
                    assert digest(rejected) == before; case['negative_cases'] += 1
                    rejected.unlink()  # reproducible from pristine + variant above
                assert digest(setup) == setup_before and digest(image) == image_before
                case['status'] = 'PASS'; manifest['cases'].append(case); print(case, flush=True)
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
