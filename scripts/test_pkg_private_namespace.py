"""Finite private-parent namespace model; not C execution or ownership validation."""
from itertools import combinations, permutations
from pathlib import Path
import hashlib
import json
import shutil
import time

ROOT = Path(__file__).resolve().parent.parent
SIZES = (20, 20, 20, 20, 16)  # three stages, intent.tmp, pending


def insert(slots, role):
    out = list(slots)
    for i, (rec, used, occupant) in enumerate(slots):
        if rec-used >= SIZES[role]:
            out[i:i+1] = ([(used, used, occupant)] if used else [])+[(rec-used, SIZES[role], role)]
            return tuple(out)
    return None


def delete(slots, role):
    return tuple((rec, 0, -1) if occupant == role else (rec, used, occupant)
                 for rec, used, occupant in slots)


def invariant(slots):
    roles = [role for _, _, role in slots if role >= 0]
    assert len(roles) == len(set(roles))
    assert sum(rec == 24 for rec, _, _ in slots) == 5
    for rec, used, role in slots:
        assert rec in (16, 24)
        assert (used == 0) if role == -1 else used == SIZES[role]
        assert not used or rec-used < 16


def main():
    out = ROOT/'.codex-remote-attachments'/('pkg-private-namespace-'+time.strftime('%Y%m%d-%H%M%S'))
    out.mkdir(parents=True)
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    paths = ['scripts/test_pkg_private_namespace.py', 'src/fs/ext4_account_namespace.inc',
             'src/fs/ext4_accounts.inc', 'src/fs/ext4_namespace.inc', 'src/lib/account_redo.c']
    sources = {name: sha(ROOT/name) for name in paths}
    for name in paths:
        dest = out/'sources'/name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT/name, dest)
        assert sha(dest) == sources[name]
    cases, edges = [], 0
    for small in (0, 1, 2):
        states = set()
        # Enumerate all unique role placements in five large slots, plus the
        # permitted case where pending occupies one of the extra 16-byte slots.
        for pending_small in range(-1, small):
            available = range(5) if pending_small == -1 else range(4)
            for count in range(len(available)+1):
                for roles in combinations(available, count):
                    for bins in permutations(range(5), count):
                        slots = [(24, 0, -1)]*5+[(16, 0, -1)]*small
                        for role, where in zip(roles, bins):
                            slots[where] = (24, SIZES[role], role)
                        if pending_small >= 0:
                            slots[5+pending_small] = (16, 16, 4)
                        states.add(tuple(slots))
        for state in states:
            invariant(state)
            present = {role for _, _, role in state if role >= 0}
            for role in range(5):
                if role not in present:
                    nxt = insert(state, role)
                    assert nxt is not None
                    invariant(nxt)
                    assert nxt in states
                    edges += 1
                else:
                    nxt = delete(state, role)
                    invariant(nxt)
                    assert nxt in states
                    edges += 1
                    for target in set(range(5))-present:
                        staged = insert(state, target)  # source still linked
                        assert staged is not None
                        invariant(staged)
                        nxt = delete(staged, role)
                        invariant(nxt)
                        assert nxt in states
                        edges += 1
            # Restart changes contents/protocol cursor, not this durable shape.
        cases.append({'small_slots': small, 'states': len(states)})
    blocked = tuple((24, 20, role) for role in range(4))
    assert insert(blocked, 4) is None
    result = {'result': 'PASS', 'scope': 'finite geometry model; C files are reference inputs only',
              'cases': cases, 'states': sum(c['states'] for c in cases), 'edges': edges,
              'four_slot_negative': 'REJECT', 'sources': sources,
              'source_preservation': all(sha(ROOT/name) == digest for name, digest in sources.items())}
    (out/'manifest.json').write_text(json.dumps(result, indent=2)+'\n')
    print('PASS', result['states'], 'shapes;', edges, 'edges; evidence:', out)


if __name__ == '__main__':
    main()
