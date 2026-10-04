"""Schedule calibration and reproducible coverage export from completed inventory."""
import hashlib
import json
from pathlib import Path
import sys
from ext4_crash_campaign import PROFILES, SCHEMA, counts, settings, tear_prefixes
from ext4_crash_model import Disk


def main():
    assert len(sys.argv) == 2
    root = Path(sys.argv[1]).resolve()
    manifest = json.loads((root / 'manifest.json').read_text())
    assert len(manifest['cases']) == 156 and not manifest['errors']
    # Independent fixed expectations for the selected-sector schedules.
    events = [{'kind': 'write', 'index': k, 'lba': k} for k in range(4)]
    expected = {'cached': (), 'write-through': (0, 1, 2, 3), 'early-low': (0,),
                'early-high': (3,), 'odd-writes': (0, 2), 'even-writes': (1, 3)}
    for ss in (512, 4096):
        for profile in PROFILES:
            config = settings(events, profile)
            assert config == settings(events, profile)
            disk = Disk(bytes(4 * ss), ss, **config)
            for k in range(4): disk.write(k, bytes([k + 1]) * ss)
            disk.restart()
            for k in range(4):
                assert disk.read(k) == bytes([k + 1 if k in expected[profile] else 0]) * ss
        assert len(tear_prefixes(ss)) == 11
    totals = {'atomic': 0, 'tears': 0, 'partial_flush': 0}
    cases = []
    for case in manifest['cases']:
        label = f'{case["block"]}-{case["placement"]}-{case["sector"]}-{case["operation"]}'
        trace_file = root / f'{label}.events.jsonl'
        trace = [json.loads(line) for line in trace_file.read_text().splitlines()]
        count = counts(trace, case['sector'])
        for name, n in count.items(): totals[name] += n
        cases.append({'label': label, 'trace_sha256': hashlib.sha256(trace_file.read_bytes()).hexdigest(),
                      'counts': count})
    plan = {'schema': SCHEMA, 'status': 'planned, not executed crash cases', 'profiles': PROFILES,
            'schedule_seed': 'none; deterministic event indices and ascending/descending LBA',
            'totals': totals, 'cases': cases,
            'limits': {'batch_cases': 2000, 'working_bytes': 2 * 1024**3, 'retained_bytes': 8 * 1024**3}}
    (root / 'coverage-plan.json').write_text(json.dumps(plan, indent=2) + '\n')
    print('Schedule calibration PASS; planned crash cases:', totals)


if __name__ == '__main__': main()
