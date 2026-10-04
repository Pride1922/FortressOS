"""Frozen Phase-9.2 schedule definitions and cut-plan counters; not a runner."""
PROFILES = ('cached', 'write-through', 'early-low', 'early-high', 'odd-writes', 'even-writes')
SCHEMA = 1


def settings(events, profile):
    if profile not in PROFILES:
        raise ValueError('persistence profile')
    dirty, scheduled = set(), {}
    writes = 0
    for event in events:
        if event['kind'] == 'flush':
            dirty.clear()
            continue
        if event['kind'] != 'write':
            raise ValueError('event kind')
        writes += 1
        dirty.add(event['lba'])
        selected = ()
        if profile in ('early-low', 'early-high') and writes % 4 == 0:
            selected = ((min if profile == 'early-low' else max)(dirty),)
        if profile == 'odd-writes' and writes % 2 or profile == 'even-writes' and not writes % 2:
            selected = (event['lba'],)
        if selected:
            scheduled[event['index']] = selected
            dirty.difference_update(selected)
    return {'write_through': profile == 'write-through', 'early_schedule': scheduled}


def tear_prefixes(sector):
    if sector not in (512, 4096):
        raise ValueError('sector size')
    return tuple(sorted({1, 2, 4, 8, 12, 16, 24, 32, sector // 2, sector - 4, sector - 1}))


def counts(events, sector):
    writes = sum(e['kind'] == 'write' for e in events)
    flushes = sum(e['kind'] == 'flush' for e in events)
    if writes + flushes != len(events):
        raise ValueError('event kind')
    return {'atomic': len(events) * 2 * len(PROFILES),
            'tears': writes * len(tear_prefixes(sector)) * 2,
            # Before-failure: no dirty sectors / lowest / highest / all descending.
            # Complete persistence with failed acknowledgement is already after.
            'partial_flush': flushes * 4 * len(PROFILES)}
