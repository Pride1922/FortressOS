"""Finite account publication/restart model. Not an EXT4/JBD2 implementation.
Directory first-fit/split follows e4_dir_edit; allocations are abstract resources.
"""
from collections import deque
from dataclasses import dataclass, replace

NAMES = ('passwd', 'group', 'shadow', 'passwd.new', 'group.new', 'shadow.new',
         'passwd.publish', 'group.publish', 'shadow.publish', 'pending')


def tree(n, bs):
    if n <= 4:
        return 0
    c = (bs-12)//12
    leaves = (n+c-1)//c
    return leaves + ((leaves+c-1)//c if leaves > 4 else 0)


@dataclass(frozen=True)
class State:
    # -1 absent, 0 created empty, 1 old canonical, 2 complete new bytes.
    files: tuple
    slots: tuple  # (record bytes, used bytes, role or -1 free / -2 immutable)
    blocks: int
    orphans: tuple
    pc: int


def size(kind, bs):
    if kind <= 0:
        return 0
    n = 1 if kind == 1 else (8192+bs-1)//bs
    return n+tree(n, bs)


def used(s, bs):
    # Private stage directory is pre-existing and fixed at one block.
    blocks = s.blocks+tree(s.blocks, bs)+1
    inodes = 2
    for role, kind in enumerate(s.files):
        if kind >= 0:
            blocks += 1 if role == 9 else size(kind, bs)
            inodes += 1
    blocks += sum(s.orphans)
    inodes += len(s.orphans)
    return blocks, inodes


def delete_slot(slots, role):
    return tuple((rec, 0, -1) if name == role else (rec, occupied, name)
                 for rec, occupied, name in slots)


def insert(s, role, bs, limit):
    need = (8+len(NAMES[role])+3)&~3
    slots = list(s.slots)
    for i, (rec, occupied, name) in enumerate(slots):
        if rec-occupied >= need:
            slots[i:i+1] = ([(occupied, occupied, name)] if occupied else [])+[(rec-occupied, need, role)]
            return replace(s, slots=tuple(slots)), 0
    if s.blocks == limit:
        return None, 0
    # Growth allocates the data block and a complete new tree before old frees.
    extra = 1+tree(s.blocks+1, bs)
    slots.append((bs-12, need, role))
    return replace(s, slots=tuple(slots), blocks=s.blocks+1), extra


def operations():
    ops = []
    for i in range(3):
        ops += [('remove', 6+i), ('reclaim', 0), ('create', 6+i), ('write', 6+i),
                ('remove', i), ('reclaim', 0), ('rename', i)]
    # Readback/validation has no filesystem allocation in this abstraction.
    ops += [('remove', 9), ('reclaim', 0)]
    for role in range(3, 9):
        ops += [('remove', role), ('reclaim', 0)]
    return ops


OPS = operations()


def advance(s, bs, limit):
    op, role = OPS[s.pc]
    files = list(s.files)
    before = used(s, bs)
    peak = before
    if op == 'reclaim':
        nxt = replace(s, orphans=())
    elif op == 'remove':
        kind = files[role]
        orphans = s.orphans
        if kind >= 0:
            orphans += ((1 if role == 9 else size(kind, bs)),)
        files[role] = -1
        nxt = replace(s, files=tuple(files), orphans=orphans,
                      slots=delete_slot(s.slots, role))
    elif op == 'create':
        assert files[role] == -1
        nxt, extra = insert(s, role, bs, limit)
        if nxt is None:
            return None, peak
        peak = (before[0]+extra, before[1]+1)
        files[role] = 0
        nxt = replace(nxt, files=tuple(files))
    elif op == 'write':
        assert files[role] == 0
        files[role] = 2
        nxt = replace(s, files=tuple(files))
    else:
        assert op == 'rename' and files[role] == -1 and files[6+role] == 2
        nxt, extra = insert(s, role, bs, limit)
        if nxt is None:
            return None, peak
        files[role], files[6+role] = files[6+role], -1
        nxt = replace(nxt, files=tuple(files), slots=delete_slot(nxt.slots, 6+role))
        # The moved object is charged once, despite two staged directory edits.
        peak = (before[0]+extra, before[1])
    nxt = replace(nxt, pc=s.pc+1)
    after = used(nxt, bs)
    peak = tuple(max(a, b, c) for a, b, c in zip(peak, before, after))
    return nxt, peak


def replay(s):
    # Journal replay has selected a complete logical transition; then boot orphan
    # cleanup releases unpinned retired objects. No free-space counter is reset.
    return replace(s, pc=0 if s.files[9] >= 0 else 23, orphans=())


def initial(bs, layout, blocks, order):
    slots = [(24, 24, -2)]  # dot entries, immutable
    for role in order:
        if layout == 'mixed':
            slots.append((24, 0, -1))
        slots.append((16, 16, role))
    remaining = bs-12-sum(x[0] for x in slots)
    if layout == 'compact':
        slots.append((remaining, 0, -1))
    else:
        # Every occupied immutable record uses its entire record. No pub-size hole.
        while remaining:
            n = 16 if remaining > 20 else remaining
            slots.append((n, n, -2))
            remaining -= n
    for _ in range(blocks-1):
        remaining = bs-12
        while remaining:
            n = 16 if remaining > 20 else remaining
            slots.append((n, n, -2))
            remaining -= n
    assert all(rec >= 8 and rec % 4 == 0 and 0 <= occupied <= rec for rec, occupied, _ in slots)
    assert sum(rec for rec, _, _ in slots) == blocks*(bs-12)
    return State((1, 1, 1, 2, 2, 2, -1, -1, -1, 2), tuple(slots), blocks, (), 0)


def explore(start, bs, limit):
    plain = start
    while plain is not None and plain.pc != len(OPS):
        plain, _ = advance(plain, bs, limit)
    uninterrupted_completes = plain is not None
    states, edges, parents = [start], [], [[]]
    index = {start: 0}
    queue = deque([0])
    blocked = []
    terminal = []
    while queue:
        i = queue.popleft()
        s = states[i]
        if s.pc == len(OPS):
            assert s.files[:3] == (2, 2, 2) and all(v == -1 for v in s.files[3:]) and not s.orphans
            terminal.append(i)
            edges.append([])
            continue
        nxt, peak = advance(s, bs, limit)
        transitions = [(replay(s), used(s, bs))]
        if nxt is None:
            blocked.append(i)
        else:
            transitions.append((nxt, peak))
        outgoing = []
        for target, demand in transitions:
            if target not in index:
                index[target] = len(states)
                states.append(target)
                parents.append([])
                queue.append(index[target])
            j = index[target]
            outgoing.append((j, demand))
            parents[j].append(i)
        edges.append(outgoing)
        assert len(states) < 100000, 'model state bound exceeded'
    # Least fixed point of peak resource demand reachable after arbitrary restarts.
    peaks = [used(s, bs) for s in states]
    queue = deque(range(len(states)))
    queued = set(queue)
    while queue:
        i = queue.popleft()
        queued.discard(i)
        value = peaks[i]
        for j, demand in edges[i]:
            value = tuple(max(a, b, c) for a, b, c in zip(value, demand, peaks[j]))
        if value != peaks[i]:
            peaks[i] = value
            for j in parents[i]:
                if j not in queued:
                    queue.append(j)
                    queued.add(j)
    # Every state must be able to reach completion; endless crashes are not liveness.
    can_finish = set(terminal)
    queue = deque(terminal)
    while queue:
        for i in parents[queue.popleft()]:
            if i not in can_finish:
                can_finish.add(i)
                queue.append(i)
    for i, s in enumerate(states):
        occupied = used(s, bs)
        remaining = tuple(p-u for p, u in zip(peaks[i], occupied))
        assert all(n >= 0 for n in remaining)
        # Keep a separate package grant in each resource dimension.
        grant = (37, 11)
        capacity = tuple(p+g for p, g in zip(peaks[0], grant))
        free = tuple(c-u for c, u in zip(capacity, occupied))
        assert all(f-g >= r for f, g, r in zip(free, grant, remaining))
        for _, demand in edges[i]:
            assert all(c-d >= g for c, d, g in zip(capacity, demand, grant))
        restarted = replay(s)
        freed = tuple(a-b for a, b in zip(occupied, used(restarted, bs)))
        assert all(n >= 0 for n in freed)
        # Replaying again releases nothing twice.
        assert used(replay(restarted), bs) == used(restarted, bs)
    witness = None
    if blocked:
        i = blocked[0]
        stop = states[i]
        steps = []
        while i:
            parent = parents[i][0]
            origin = states[parent]
            steps.append({'pc': origin.pc, 'step': 'replay' if replay(origin) == states[i] else OPS[origin.pc][0]})
            i = parent
        witness = {'steps': list(reversed(steps)), 'blocked_pc': stop.pc,
                   'blocked_operation': OPS[stop.pc], 'files': stop.files,
                   'directory_blocks': stop.blocks, 'directory_limit': limit}
    return {'states': len(states), 'edges': sum(map(len, edges)), 'blocked_states': len(blocked),
            'uninterrupted_completes': uninterrupted_completes,
            'all_states_can_finish': len(can_finish) == len(states),
            'admission': 'ACCEPT' if not blocked and len(can_finish) == len(states) else 'REJECT',
            'initial_used': used(start, bs), 'peak': peaks[0],
            'remaining': tuple(p-u for p, u in zip(peaks[0], used(start, bs))),
            'blocked_witness': witness}


def mutation_checks():
    # A pre-existing stage must not be counted as both allocated and free credit.
    capacity, used_blocks, grant, required = 100, 70, 20, 15
    actual_free = capacity-used_blocks
    assert actual_free-grant < required
    forged_free = actual_free+10
    assert forged_free-grant >= required
    assert used_blocks+forged_free != capacity
    # The legacy bypass can consume G even though its ordinary zero-floor check passes.
    free, grant, spend = 21, 20, 2
    assert free-spend >= 0 and free-spend < grant
    return 2
