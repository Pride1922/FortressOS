#!/usr/bin/env python3
"""Validate plain/wait-only/phase cohorts; attribute scheduler elapsed waits."""
import argparse
import hashlib
import json
import re
import statistics
from pathlib import Path
from analyze_smp_profile import fields
from test_smpbench_qemu import validate_barrier, validate_profile

def validate_pipe_profile(workers, workload, phase):
    if not any('pi_diag' in w for w in workers):
        return
    assert workload in ('pipes', 'pipe_bw', 'pipes4k')
    for w in workers:
        assert w.get('pi_diag') == '1' and w.get('pi_valid') == '1'
        v = {key[3:]: int(value) for key, value in w.items() if key.startswith('pi_')}
        assert all(0 <= value < (1 << 64) for value in v.values())
        assert v['read_bytes'] == v['write_bytes'] == int(w['iterations'])*1024 + 8 + (104 if phase else 0)
        for direction in ('read', 'write'):
            calls = v[direction+'s']
            assert calls == sum(v[direction+'_'+bucket] for bucket in ('small','1k','large'))
            assert 0 < v[direction+'_max'] <= 65536 and calls > 0
            assert 0 < v['reader_cpus' if direction == 'read' else 'writer_cpus'] < (1 << 64)
        assert v['empty_drains'] <= v['reads']
        assert v['empty_fills'] <= v['writes'] and v['full_fills'] <= v['writes']
        assert v['direction_changes'] < v['reads']+v['writes']
        assert v['cpu_changes'] < v['reads']+v['writes']

def analyze(path):
    text = re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]', '', path.read_text()).replace('\r','')
    summaries = [fields(line) for line in text.splitlines() if line.startswith('w=')]
    assert len(summaries) == 1, f'Expected one complete workload: {path}'
    summary = summaries[0]
    assert summary['ok'] == '1' and summary['short'] == '0'
    n, reps = int(summary['n']), int(summary['reps'])
    phase = 'smpbench rev=3 profile=1 ' in text
    (validate_profile if phase else validate_barrier)(text,n,reps)
    workers = [fields(line) for line in text.splitlines() if line.startswith('smpbench_worker ')]
    validate_pipe_profile(workers, summary['w'], phase)
    trace = all(w.get('wait_diag') == '1' for w in workers)
    assert trace or all('wait_diag' not in w for w in workers), 'Partial wait evidence'
    records = []
    for rep in range(1,reps+1):
        cohort = [w for w in workers if int(w['rep']) == rep]
        critical = max(cohort,key=lambda w:int(w['time_us']))
        wait = None
        if trace:
            hz = int(critical['wait_hz'])
            wait = {'blocks':int(critical['wait_blocks']),
                    **{name+'_us':int(critical['wait_'+name])*1000000/hz
                       for name in ('blocked','ready','resume','ready_max')}}
        records.append({'rep':rep,'critical_worker':int(critical['worker_id']),
                        'elapsed_us':int(critical['time_us']),'wait':wait,'workers':cohort})
    elapsed = sorted(r['elapsed_us'] for r in records)
    for name, value in (('min', elapsed[0]), ('med', elapsed[reps//2]), ('max', elapsed[-1])):
        if name+'_us' in summary:
            assert value == int(summary[name+'_us'])
        else:
            # Human-readable mode rounds the summary to 0.1 ms; worker
            # records still carry exact microseconds. Verify before deriving.
            rounded = (value + 50) // 100
            assert summary[name] == f'{rounded//10}.{rounded%10}ms'
            summary[name+'_us'] = str(value)
    return {'log':str(path.resolve()),'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
            'mode':'phase' if phase else 'wait-only' if trace else 'plain',
            'summary':summary,'repetitions':records}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs',nargs='+',type=Path)
    parser.add_argument('--output',required=True,type=Path)
    args = parser.parse_args()
    assert not args.output.exists(), 'Choose a new evidence output'
    records = [analyze(p) for p in args.logs]
    args.output.write_text(json.dumps({'scope':'Elapsed scheduler waits including all worker waits; warmup excluded','records':records},indent=2))
    for record in records:
        rows = record['repetitions']
        attribution = {name:round(statistics.mean(r['wait'][name] for r in rows),1)
                       for name in ('blocked_us','ready_us','resume_us','ready_max_us')} if rows[0]['wait'] else {}
        print(Path(record['log']).name,record['mode'],'median_us='+record['summary']['med_us'],attribution)

if __name__ == '__main__':
    main()
