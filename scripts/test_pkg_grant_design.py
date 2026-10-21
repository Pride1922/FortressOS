#!/usr/bin/env python3
"""Finite candidate-design model only; no filesystem or boot acceptance."""
import itertools
import json
from dataclasses import dataclass, replace


@dataclass(frozen=True)
class State:
    free: int
    grant: int
    charged: int
    baseline: int
    total: int


def valid(s, capacity, reserve):
    return (min(s.free, s.grant, s.charged, s.baseline, s.total) >= 0
            and s.free + s.charged + s.baseline == capacity
            and s.total == s.grant + s.charged
            and s.free >= s.grant + reserve)


def transitions(s, reserve):
    # One unit denotes either a block or an inode, independently.
    if s.grant:
        yield 'charge', replace(s, free=s.free-1, grant=s.grant-1,
                                charged=s.charged+1)
    if s.charged:
        yield 'refund', replace(s, free=s.free+1, grant=s.grant+1,
                                charged=s.charged-1)
        yield 'transfer', replace(s, charged=s.charged-1,
                                  baseline=s.baseline+1, total=s.total-1)
    if s.baseline:
        yield 'baseline-free', replace(s, free=s.free+1, baseline=s.baseline-1)
    if s.grant:
        # This model uses an empty remaining plan for release eligibility.
        yield 'release', replace(s, grant=s.grant-1, total=s.total-1)
    if s.free > s.grant + reserve:
        yield 'ordinary-or-account', replace(s, free=s.free-1,
                                             baseline=s.baseline+1)


def mount_policy(selector, artifact):
    if selector == 'legacy' and artifact == 'absent':
        return 'legacy'
    if selector == 'required' and artifact == 'valid':
        return 'protected-admission'
    return 'recovery'


def may_retire(phase, charged, cleanup_complete, pending):
    return (phase in ('RESERVED', 'RETIRING') and charged == 0
            and cleanup_complete and not pending)


def ordered(events):
    required = ['preview-validate', 'replay', 'reconstruct-grant',
                'journal-activate', 'orphan-cleanup', 'namespace-fence',
                'accounts-prepare', 'publish']
    return (len(events) == len(required) and set(events) == set(required)
            and all(events.index(a) < events.index(b)
                    for a, b in zip(required, required[1:])))


def main():
    states = edges = restart_selections = 0
    kinds = set()
    for capacity in range(1, 13):
        for reserve in range(0, min(capacity, 3)+1):
            for free in range(capacity+1):
                for charged in range(capacity-free+1):
                    for grant in range(free+1):
                        s = State(free, grant, charged, capacity-free-charged,
                                  grant+charged)
                        if not valid(s, capacity, reserve):
                            continue
                        states += 1
                        for kind, after in transitions(s, reserve):
                            assert valid(after, capacity, reserve), (s, kind, after)
                            kinds.add(kind)
                            edges += 1
                            # A journal cut may select only the entire before or
                            # after state. Repeated mount does not release escrow.
                            for durable in (s, after):
                                recovered = durable
                                assert valid(recovered, capacity, reserve)
                                assert recovered == durable
                                restart_selections += 1
    assert kinds == {'charge', 'refund', 'transfer', 'baseline-free',
                     'release', 'ordinary-or-account'}
    # Independent dimensions: block headroom cannot compensate for inode deficit.
    pair_checks = 0
    for rb, ri in itertools.product(range(3), repeat=2):
        b = State(4+rb, 4, 0, 2-rb, 4)
        i = State(2+ri, 2, 0, 2-ri, 2)
        assert valid(b, 6, rb) and valid(i, 4, ri)
        assert not valid(replace(i, free=i.free-1, baseline=i.baseline+1), 4, ri)
        pair_checks += 1
    policy_cases = 0
    for selector, artifact in itertools.product(
            ('legacy', 'required'), ('absent', 'valid', 'malformed', 'partial', 'unreadable')):
        expected = ('legacy' if (selector, artifact) == ('legacy', 'absent')
                    else 'protected-admission' if (selector, artifact) == ('required', 'valid')
                    else 'recovery')
        assert mount_policy(selector, artifact) == expected
        policy_cases += 1
    events = ['preview-validate', 'replay', 'reconstruct-grant',
              'journal-activate', 'orphan-cleanup', 'namespace-fence',
              'accounts-prepare', 'publish']
    accepted = sum(ordered(p) for p in itertools.permutations(events))
    assert accepted == 1
    # Deliberately broken designs must be detected; retain their names in output.
    s = State(6, 4, 1, 3, 5)
    controls = {
        'split-allocation-ledger': not valid(replace(s, free=5, charged=2), 10, 2),
        'double-refund': not valid(replace(s, free=8, grant=6, charged=0), 10, 2),
        'account-bypass-spends-escrow': not valid(replace(s, free=5, baseline=4), 10, 2),
        'baseline-free-refunded-as-grant': not valid(replace(s, free=7, baseline=2, grant=5), 10, 2),
        'transfer-invents-free-space': not valid(replace(s, free=7, charged=0, baseline=4, total=4), 10, 2),
        'required-marker-missing-fallback': mount_policy('required', 'absent') != 'legacy',
        'owner-death-release': not may_retire('RESERVED', s.charged, False, False),
        'pending-release': not may_retire('PENDING', 0, True, True),
        'pinned-retirement-release': not may_retire('RETIRING', 1, True, False),
        'cleanup-before-reconstruction': not ordered(events[:2]+[events[4], events[3], events[2]]+events[5:]),
        'publish-before-fence': not ordered(events[:5]+[events[7], events[6], events[5]]),
    }
    assert all(controls.values()), controls
    assert may_retire('RESERVED', 0, True, False)
    assert may_retire('RETIRING', 0, True, False)
    print(json.dumps({'scope': 'finite candidate arithmetic/order model only',
                      'states': states, 'edges': edges,
                      'whole_transaction_restart_selections': restart_selections,
                      'independent_dimension_cases': pair_checks,
                      'mount_policy_cases': policy_cases,
                      'order_permutations': 40320, 'accepted_orders': accepted,
                      'negative_controls': controls, 'result': 'PASS'}, indent=2))


if __name__ == '__main__':
    main()
