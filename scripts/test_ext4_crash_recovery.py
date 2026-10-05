"""Phase 9.2 bounded, retained campaign with actual sanitizer recovery worker."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import struct
import subprocess
import tempfile
import time
from create_ext4_fixtures import ROOT
from ext4_crash_campaign import PROFILES, settings, tear_prefixes, counts
from ext4_crash_model import Disk, Disconnected, Fault
from ext4_crash_sparse import SparseDisk, digest
from ext4_crash_recovery_oracle import Truth
from test_jbd2_replay_host import blocks
from test_ext4_crash_inventory import OPS, linux_snapshot, command


def read_exact(pipe, size, deadline):
    result = bytearray()
    while len(result) < size:
        remaining = deadline-time.monotonic()
        if remaining <= 0 or not select.select([pipe], [], [], remaining)[0]:
            raise TimeoutError('recovery worker deadline')
        chunk = os.read(pipe.fileno(), size-len(result))
        if not chunk: raise RuntimeError('recovery worker exited')
        result.extend(chunk)
    return bytes(result)


def faults(events, ss, profile):
    config = settings(events, profile)
    # Dirty sets are taken immediately before each flush, with early persistence.
    dirty = set()
    for event in events:
        index = event['index']
        yield 'atomic', Fault(index), config
        yield 'atomic', Fault(index, after=True), config
        if event['kind'] == 'write':
            if profile in ('cached', 'write-through'):
                for prefix in tear_prefixes(ss): yield 'tears', Fault(index, tear_bytes=prefix), config
            dirty.add(event['lba'])
            if config['write_through']: dirty.discard(event['lba'])
            dirty.difference_update(config['early_schedule'].get(index, ()))
        else:
            sectors = sorted(dirty)
            for choice in ((), tuple(sectors[:1]), tuple(sectors[-1:]), tuple(reversed(sectors))):
                yield 'partial_flush', Fault(index, flush_sectors=choice), config
            dirty.clear()


def write_image(path, base, delta, ss):
    with path.open('wb') as stream:
        stream.write(base)
        for at in range(0, len(delta), 4+ss):
            lba = struct.unpack_from('<I', delta, at)[0]
            stream.seek(lba*ss);stream.write(delta[at+4:at+4+ss])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('inventory', type=Path)
    parser.add_argument('--label', default=None)
    parser.add_argument('--operation', choices=OPS, default=None)
    parser.add_argument('--limit', type=int, default=None)
    args = parser.parse_args()
    source = args.inventory.resolve(); allowed = (ROOT/'.codex-remote-attachments').resolve()
    assert source.is_relative_to(allowed)
    inventory = json.loads((source/'manifest.json').read_text())
    operations=tuple(inventory.get('operations',OPS))
    assert operations in (OPS,('write',)) and not inventory['errors']
    declared={f'{bs}-{placement}-{ss}-{op}' for bs in (1024,2048,4096)
              for placement in ('normal','wrap') for ss in (512,4096) for op in operations}
    actual=[f'{b["block"]}-{b["placement"]}-{b["sector"]}-{b["operation"]}' for b in inventory['cases']]
    assert len(actual)==len(declared) and set(actual)==declared
    evidence = Path(os.environ.get('FORTRESS_EXT4_CRASH_EVIDENCE', allowed/'ext4-phase9')).resolve()
    assert evidence.is_relative_to(allowed)
    out = Path(tempfile.mkdtemp(prefix='campaign-', dir=evidence))
    binary = evidence/'bin/ext4_crash_recover_host'
    manifest = {'scope': 'Phase 9.2', 'argv': os.sys.argv, 'inventory': str(source), 'cases': [], 'errors': [],
                'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest()}
    snapshot = out/'source-snapshot';snapshot.mkdir()
    paths = [ROOT/'tests/ext4_crash_recover_host.c', ROOT/'tests/ext4_integration_host.c',
             ROOT/'tests/ext4_mount_host.c', ROOT/'tests/ext4_fault_disk.c',
             *sorted((ROOT/'src/fs').glob('ext4*')), ROOT/'src/fs/jbd2.c', ROOT/'src/fs/jbd2.h',
             Path(__file__), ROOT/'scripts/ext4_crash_sparse.py', ROOT/'scripts/ext4_crash_recovery_oracle.py',
             ROOT/'scripts/ext4_crash_campaign.py', ROOT/'scripts/test_ext4_crash_inventory.py']
    manifest['sources'] = {}
    for path in paths:
        if not path.is_file(): continue
        relative = path.relative_to(ROOT); target = snapshot/relative
        target.parent.mkdir(parents=True, exist_ok=True);shutil.copyfile(path,target)
        manifest['sources'][str(relative)] = hashlib.sha256(path.read_bytes()).hexdigest()
    shutil.copyfile(binary, snapshot/'recovery-worker')
    total = 0; started = time.monotonic(); batch_started = started
    try:
        for baseline in inventory['cases']:
            label = f'{baseline["block"]}-{baseline["placement"]}-{baseline["sector"]}-{baseline["operation"]}'
            if args.label and label != args.label: continue
            if args.operation and baseline['operation'] != args.operation: continue
            bs, ss, op = baseline['block'], baseline['sector'], baseline['operation']
            folder = out/label; folder.mkdir(); work = folder/'working.img'
            base = gzip.decompress((source/f'{label}-before.img.gz').read_bytes())
            preimage = next(i for i in baseline['images'] if i['file']==f'{label}-before.img.gz')
            assert hashlib.sha256(base).hexdigest()==preimage['sha256'], 'changed immutable preimage'
            assert len(base) < 2*1024**3 and shutil.disk_usage(out).free > 2*len(base)
            work.write_bytes(base)
            journal = blocks(work, f'<{struct.unpack_from("<I",base,1248)[0]}>')
            events = [json.loads(line) for line in (source/f'{label}.events.jsonl').read_text().splitlines()]
            payload = (source/f'{label}.payload.bin').read_bytes()
            assert hashlib.sha256(payload).hexdigest()==baseline['payload_sha256'], 'changed event payload'
            truth = Truth(base, ss, bs, events, payload, journal)
            cache, linux_cache = {}, {}
            attempted = {'atomic': 0, 'tears': 0, 'partial_flush': 0}
            outcomes = {'recovered': 0, 'rejected': 0, 'unique_inputs': 0, 'linux_audits': 0}
            stderr = (folder/'worker.log').open('wb')
            basefile = folder/'base.img'; basefile.write_bytes(base)
            worker = subprocess.Popen([str(binary), str(basefile), str(ss)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr)
            ledger = (folder/'cases.jsonl').open('w')
            try:
                for profile in PROFILES:
                    for kind, fault, config in faults(events, ss, profile):
                        if args.limit and total >= args.limit: break
                        disk = SparseDisk(base, ss, **config)
                        for event in events[:fault.event+1]:
                            try: disk.apply(event, payload, fault if event['index']==fault.event else None)
                            except Disconnected: break
                        delta = disk.canonical(); key = digest(delta)
                        expected, witness = truth.expected(disk)
                        case = {'profile': profile, 'kind': kind, 'fault': fault.__dict__, 'input_delta_sha256': key,
                                'expected': expected, 'witness': witness, 'batch': total//2000}
                        # Every byte-identical stable state has one fresh actual recovery.
                        # Include expectation in cache consistency, never in media identity.
                        if key in cache:
                            assert cache[key]['expected'] == expected, 'inconsistent durable truth'
                            result = cache[key]
                        else:
                            (folder/'current-case.json').write_text(json.dumps(case, indent=2)+'\n')
                            (folder/'current-input.delta').write_bytes(delta)
                            count = len(delta)//(4+ss)
                            worker.stdin.write(struct.pack('<III', count, OPS.index(op), expected)+delta);worker.stdin.flush()
                            deadline = time.monotonic()+180
                            header = read_exact(worker.stdout, 20, deadline)
                            status, error, writes, flushes, changed = struct.unpack('<IIIII', header)
                            recovered = read_exact(worker.stdout, changed*(4+ss), deadline)
                            assert status == int(expected==2)
                            result = {'expected': expected, 'status': status, 'return_code': error,
                                      'writes': writes, 'flushes': flushes, 'output_delta_sha256': digest(recovered)}
                            if not status:
                                # Exact home-media equivalence permits Linux audit reuse;
                                # journal data are excluded only after clean/empty proof in C.
                                home = bytearray()
                                for at in range(0, len(recovered), 4+ss):
                                    lba = struct.unpack_from('<I', recovered, at)[0]
                                    value = recovered[at+4:at+4+ss]
                                    for skip in range(0, ss, min(bs, ss)):
                                        block = (lba*ss+skip)//bs
                                        if block not in journal: home += struct.pack('<QI', lba*ss+skip, min(bs, ss))+value[skip:skip+min(bs, ss)]
                                home_key = digest(home)
                                if home_key not in linux_cache:
                                    write_image(work, base, recovered, ss)
                                    linux_snapshot(work, bs, op, folder/f'linux-{home_key}.log', committed=expected==1)
                                    linux_cache[home_key] = {'image_sha256': hashlib.sha256(work.read_bytes()).hexdigest()}
                                    (folder/f'linux-{home_key}.delta').write_bytes(recovered)
                                    outcomes['linux_audits'] += 1
                                result['linux_home_sha256'] = home_key
                            cache[key] = result; outcomes['unique_inputs'] += 1
                        ledger.write(json.dumps(dict(case, result=result))+'\n')
                        attempted[kind] += 1; outcomes['rejected' if expected==2 else 'recovered'] += 1; total += 1
                        if total % 2000 == 0:
                            assert time.monotonic()-batch_started < 300, 'batch deadline exceeded'
                            batch_started = time.monotonic()
                            ledger.flush(); print(f'progress cases={total} unique={outcomes["unique_inputs"]} label={label}', flush=True)
                        if args.limit and total >= args.limit: break
                    if args.limit and total >= args.limit: break
            finally:
                ledger.close(); worker.stdin.close()
                try: code = worker.wait(timeout=10)
                except subprocess.TimeoutExpired: worker.kill();code=worker.wait(timeout=10)
                stderr.close()
                if code and not manifest['errors']: manifest['errors'].append(f'worker return code {code}: {label}')
            expected_counts = counts(events, ss)
            complete = attempted == expected_counts
            assert complete or args.limit, (label, attempted, expected_counts)
            manifest['cases'].append({'label': label, 'attempted': attempted, 'expected_counts': expected_counts,
                                      'complete': complete, 'outcomes': outcomes, 'seconds': time.monotonic()-started})
            print(f'PASS {label}: {attempted}, {outcomes}', flush=True)
            # Canonical preimages remain gzip-retained in the immutable inventory.
            # Keep full working images on failures, not for successful duplicate states.
            basefile.unlink(); work.unlink(missing_ok=True)
            assert sum(p.stat().st_size for p in out.rglob('*') if p.is_file()) < 8*1024**3, 'retained evidence budget exceeded'
            if args.limit and total >= args.limit: break
        assert manifest['cases'] and not manifest['errors'], manifest['errors']
    except Exception as error:
        if 'delta' in locals(): write_image(folder/'failure-input.img', base, delta, ss)
        manifest['errors'].append(repr(error));raise
    finally:
        manifest['total'] = total;manifest['seconds'] = time.monotonic()-started
        (out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
        print(f'Evidence: {out}', flush=True)


if __name__ == '__main__': main()
