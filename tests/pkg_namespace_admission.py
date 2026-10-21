"""Conservative /etc slot admission candidate; test specification only.
Input role identities/ownership must already have been authenticated by caller.
No production mount scanner, writer fencing or resource admission is provided.
"""
from collections import deque
from dataclasses import replace
from itertools import permutations, product

from pkg_account_replay_model import State, OPS, advance, replay

ROLES = (0, 1, 2, 6, 7, 8)


def admit(slots):
    bins, seen = 0, set()
    for rec, used, role in slots:
        if rec < 12 or rec % 4 or used < 0 or used > rec or used % 4:
            return False
        if role == -1:
            if used or rec not in (12, 16, 24):
                return False
            bins += rec == 24
        elif role == -2:
            if used < 12 or rec-used >= 16:
                return False
        elif role in ROLES:
            if role in seen or used != (16 if role < 3 else 24):
                return False
            if rec not in ((16, 24) if role < 3 else (24,)):
                return False
            seen.add(role)
            bins += rec == 24
        else:
            return False
    return bins >= 6


def seed(bs, count, placement, order):
    canonical = [(16, 16, r) for r in order]
    bins = [(24, 0, -1)]*count
    if placement == 'before':
        slots = bins+canonical
    elif placement == 'after':
        slots = canonical+bins
    else:
        slots = []
        for i, item in enumerate(canonical):
            slots += bins[i*count//3:(i+1)*count//3]+[item]
    slots = [(24, 24, -2)]+slots
    remaining = bs-12-sum(r for r, _, _ in slots)
    while remaining:
        rec = 16 if remaining > 20 else remaining
        slots.append((rec, rec, -2))
        remaining -= rec
    assert all(rec >= 12 for rec, _, _ in slots)
    return State((1, 1, 1, 2, 2, 2, -1, -1, -1, 2), tuple(slots), 1, (), 0)


def check(start, bs):
    accepted = admit(start.slots)
    queue, seen = deque([start]), {start}
    blocked, completed, transitions = 0, 0, 0
    while queue:
        state = queue.popleft()
        if accepted:
            assert admit(state.slots), 'admission must survive every transition and restart'
        roles = [role for _, _, role in state.slots if role in ROLES]
        assert sorted(roles) == sorted(role for role in ROLES if state.files[role] >= 0)
        if state.pc == len(OPS):
            completed += 1
            continue
        nxt, _ = advance(state, bs, 1)  # no growth credit
        if nxt is None:
            blocked += 1
            assert not accepted
        successors = [replay(state)]+([nxt] if nxt else [])
        for target in successors:
            transitions += 1
            if target not in seen:
                seen.add(target)
                queue.append(target)
        assert len(seen) < 100000
    if accepted:
        assert not blocked and completed
    return dict(admitted=accepted, states=len(seen), edges=transitions,
                blocked_states=blocked, terminal_states=completed)


def run():
    records = []
    for bs, count, placement, order in product((1024, 2048, 4096), (3, 4, 5, 6),
                                              ('before', 'after', 'mixed'), permutations(range(3))):
        start = seed(bs, count, placement, order)
        result = check(start, bs)
        assert result['admitted'] == (count == 6)
        records.append(dict(block_size=bs, bins=count, placement=placement, order=order, **result))
    assert any(r['bins'] == 5 and r['blocked_states'] for r in records)
    good = seed(1024, 6, 'before', (0, 1, 2)).slots
    assert admit(good)
    bad = [good+((24, 24, 0),), good+((24, 16, 6),), good+((24, 0, 3),),
           good+((48, 0, -1),), good+((32, 12, -2),), good+((8, 0, -1),)]
    assert all(not admit(slots) for slots in bad)
    # An excluded concurrent writer can steal a bin; writer fencing is required.
    stolen = list(good)
    index = stolen.index((24, 0, -1))
    stolen[index] = (24, 24, -2)
    assert not admit(tuple(stolen))
    return {'result': 'PASS', 'cases': records, 'negative_controls': len(bad)+1}
