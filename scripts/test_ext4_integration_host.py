"""Phase 8.6 mounted transaction coverage and operation-boundary recovery gate."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import struct
import tempfile
from create_ext4_fixtures import ROOT, run
from test_jbd2_replay_host import oracle, blocks, be, crc

EVIDENCE = Path(os.environ.get('FORTRESS_EXT4_INTEGRATION_EVIDENCE',
    ROOT / '.codex-remote-attachments/ext4-phase8-6'))
STAGING_BINARY=Path(os.environ.get('FORTRESS_EXT4_STAGING_BINARY',EVIDENCE/'bin/ext4_integration_staging'))
OPERATIONS = ('create', 'mkdir', 'write', 'append', 'rename', 'truncate', 'unlink',
    'open-unlink', 'last-close', 'rmdir', 'reuse', 'sync', 'freeze')

def clean_audit(image, bs):
    data=image.read_bytes()
    assert struct.unpack_from('<H',data,1082)[0]==1
    assert struct.unpack_from('<I',data,1120)[0]==0x42
    assert struct.unpack_from('<I',data,1256)[0]==0
    assert struct.unpack_from('<I',data,2044)[0]==crc(0xffffffff,data[1024:2044])
    journal=blocks(image,f'<{struct.unpack_from("<I",data,1248)[0]}>')
    js=bytearray(data[journal[0]*bs:journal[0]*bs+1024]);assert not be(js,28)
    stored=be(js,252);struct.pack_into('>I',js,252,0)
    assert stored==crc(0xffffffff,js)

def record_audits(prefix,bs):
    checked=[]
    for suffix in ('shared-write','independent-append'):
        image=Path(str(prefix)+'-'+suffix+'.img');linux=Path(str(image)+'.linux.img')
        log=Path(str(image)+'.linux.log');oracle(image,linux,log);clean_audit(image,bs)
        dump=Path(str(image)+'.bytes');dump.unlink(missing_ok=True)
        text=run(['debugfs','-R',f'dump /records.bin {dump}',str(linux)])
        data=dump.read_bytes();assert len(data)==2048
        records=[data[k:k+32] for k in range(0,len(data),32)]
        expected={bytes([wid,seq])+bytes([65+wid])*30 for wid in range(2) for seq in range(32)}
        assert set(records)==expected
        with log.open('a') as stream:stream.write(text)
        checked.append({'image':str(image),'sha256':hashlib.sha256(image.read_bytes()).hexdigest()})
    return checked

def profile(source, bs, ss, wrap, out, staging=False):
    label = f'{bs}-{"wrap" if wrap else "normal"}-{ss}'
    prefix = out / label
    cmd = [str(STAGING_BINARY if staging else EVIDENCE/'bin/ext4_integration_host'),
        str(source), str(ss), str(prefix)]
    if staging:cmd.append('--staging')
    with (out / f'{label}.log').open('w') as stream:
        result = subprocess.run(cmd, stdout=stream, stderr=subprocess.STDOUT, timeout=900)
    text = (out / f'{label}.log').read_text()
    assert result.returncode == 0, f'retained {prefix}.log: {text[-2000:]}'
    match = re.search(r'EXT4 integration PASS block=\d+ sector=\d+ cuts=(\d+)', text)
    assert match, text
    if staging:
        stages=int(re.search(r'stages=(\d+)',text)[1]);audits=record_audits(prefix,bs)
        print(f'PASS {label}: {stages} staging/credit injections, shared/independent records',flush=True)
        return {'block':bs,'sector':ss,'wrap':wrap,'argv':cmd,'stages':stages,'cuts':0,'audits':audits,
            'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest()}
    audits = []
    for op in OPERATIONS:
        image = Path(str(prefix) + '-' + op + '.img')
        linux = Path(str(image) + '.linux.img')
        log = Path(str(image) + '.linux.log')
        oracle(image, linux, log)
        clean_audit(image,bs)
        path = '/sub/renamed.bin' if op == 'rename' else '/reuse.bin' if op == 'reuse' else '/target.bin'
        payload = b'I' * (2 * bs) if op == 'reuse' else b'O' * (bs + 17) if op == 'truncate' \
            else b'O' * (3 * bs) + b'I' * (2 * bs) if op == 'write' \
            else b'O' * (3 * bs) + b'I' * 17 if op == 'append' else b'O' * (3 * bs)
        if op not in ('unlink', 'open-unlink', 'last-close'):
            dump = Path(str(image) + '.bytes');dump.unlink(missing_ok=True)
            text = run(['debugfs', '-R', f'dump {path} {dump}', str(linux)])
            assert dump.read_bytes() == payload, (label, op)
            with log.open('a') as stream:stream.write(text)
        for absent in (['/target.bin'] if op == 'rename' else ['/sub'] if op == 'rmdir' else []):
            assert 'File not found' in run(['debugfs', '-R', f'stat {absent}', str(linux)])
        if op in ('unlink','open-unlink','last-close','reuse'):
            assert 'File not found' in run(['debugfs','-R','stat /target.bin',str(linux)])
        if op in ('create','mkdir'):
            name='/new.bin' if op=='create' else '/newdir'
            assert 'Type: '+('regular' if op=='create' else 'directory') in run(['debugfs','-R',f'stat {name}',str(linux)])
        audits.append({'operation': op, 'sha256': hashlib.sha256(image.read_bytes()).hexdigest()})
    audits+=record_audits(prefix,bs)
    print(f'PASS {label}: mounted coverage, {match[1]} boundary cuts, 15 Linux operation audits', flush=True)
    return {'block': bs, 'sector': ss, 'wrap': wrap, 'argv': cmd, 'cuts': int(match[1]),
        'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(), 'audits': audits}

def main():
    assert len(os.sys.argv) in (2,3), 'usage: test_ext4_integration_host.py completed-phase85-host-directory [--staging]'
    staging=len(os.sys.argv)==3
    if staging:assert os.sys.argv[2]=='--staging'
    fixtures = Path(os.sys.argv[1]).resolve()
    allowed = (ROOT / '.codex-remote-attachments').resolve()
    assert fixtures.is_relative_to(allowed) and (fixtures / 'manifest.json').is_file()
    EVIDENCE.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix='staging-' if staging else 'host-', dir=EVIDENCE))
    jobs = [(fixtures / f'{bs}-{"wrap" if wrap else "normal"}.img', bs, ss, wrap, out,staging)
        for bs in (1024, 2048, 4096) for wrap in (False, True) for ss in (512, 4096)]
    records = [];errors = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        pending = [pool.submit(profile, *args) for args in jobs]
        for future in concurrent.futures.as_completed(pending):
            try:records.append(future.result())
            except Exception as error:errors.append(str(error));print(f'FAIL {error}', flush=True)
    (out / 'manifest.json').write_text(json.dumps({'cases': records, 'errors': errors,
        'binary_sha256': hashlib.sha256((STAGING_BINARY if staging else EVIDENCE/'bin/ext4_integration_host').read_bytes()).hexdigest()}, indent=2) + '\n')
    assert not errors, errors
    print(f'EXT4 Phase 8.6 host PASS 12/12, cuts={sum(r["cuts"] for r in records)}, staging={sum(r.get("stages",0) for r in records)}; evidence: {out}')

if __name__ == '__main__':main()
