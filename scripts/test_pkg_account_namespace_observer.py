"""Read-only slot/allocation observations for a retained namespace campaign."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct

from test_jbd2_replay_host import crc

ROOT = Path(__file__).resolve().parent.parent


def observe(campaign):
    campaign = campaign.resolve()
    assert campaign.is_relative_to((ROOT/'.codex-remote-attachments').resolve())
    manifest = json.loads((campaign/'manifest.json').read_text())
    assert manifest['result'] == 'PASS'
    results = []
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    for case in manifest['cases']:
        name = case['name']
        image = campaign/(name+'.img')
        assert sha(image) == case['image_sha256']
        bs = int(name.split('-')[0])
        folder = campaign/str(bs)
        mapping = [int(n) for n in re.findall(r'^(\d+)\s*$', (folder/'mapping.log').read_text(), re.M)]
        text = (folder/'fixture.log').read_text()
        # First stat /etc follows the debugfs initialization log.
        match = re.search(r'Inode:\s+(\d+)\s+Type:\s+directory.*?Generation:\s+(\d+)', text, re.S)
        assert match
        ino, generation = map(int, match.groups())
        disk = image.read_bytes()
        u32 = lambda off: struct.unpack_from('<I', disk, off)[0]
        seed = crc(crc(crc(0xffffffff, disk[1128:1144]), struct.pack('<I', ino)),
                   struct.pack('<I', generation))
        free_lengths, roles = {}, {}
        for logical, physical in enumerate(mapping):
            block = disk[physical*bs:(physical+1)*bs]
            assert crc(seed, block[:-12]) == struct.unpack_from('<I', block, bs-4)[0]
            at = 0
            while at < bs-12:
                inode, rec, length, kind = struct.unpack_from('<IHBB', block, at)
                assert rec >= 12 and rec % 4 == 0 and at+rec <= bs-12
                label = block[at+8:at+8+length].decode() if inode else ''
                used = (8+length+3) & ~3 if inode else 0
                if label in ('passwd', 'group', 'shadow', 'passwd.publish', 'group.publish', 'shadow.publish'):
                    assert label not in roles
                    roles[label] = {'logical_block': logical, 'offset': at, 'inode': inode,
                                    'record_bytes': rec, 'used_bytes': used, 'type': kind}
                room = rec-used
                if room:
                    free_lengths[room] = free_lengths.get(room, 0)+1
                at += rec
            assert at == bs-12
        available = sum(n for size, n in free_lengths.items() if size >= 24)
        if '-group' in name or '-blocked' in name:
            assert available == 0 and 'passwd.publish' not in roles
            assert 'group.publish' in roles and 'shadow.publish' in roles
        groups = (u32(1028)-u32(1044)+u32(1056)-1)//u32(1056)
        allocated = {'blocks': [], 'inodes': []}
        implicit = []
        for group in range(groups):
            desc = (u32(1044)+1)*bs+group*32
            for label, offset, per_group, total, first in (
                    ('blocks', 0, u32(1056), u32(1028), u32(1044)),
                    ('inodes', 4, u32(1064), u32(1024)+1, 1)):
                bitmap = disk[u32(desc+offset)*bs:(u32(desc+offset)+1)*bs]
                start = first+group*per_group
                count = min(per_group, total-start)
                flags = struct.unpack_from('<H', disk, desc+18)[0]
                free = struct.unpack_from('<H', disk, desc+(12 if label == 'blocks' else 14))[0]
                if flags & (2 if label == 'blocks' else 1):
                    assert label == 'blocks' or free == count
                    implicit.append({'group': group, 'kind': label, 'start': start,
                                     'count': count, 'allocated_count': count-free,
                                     'positions': 'NOT ENUMERATED: bitmap UNINIT'})
                    continue
                before = len(allocated[label])
                for bit in range(count):
                    if bitmap[bit//8] & (1 << (bit % 8)):
                        allocated[label].append(start+bit)
                assert count-(len(allocated[label])-before) == free
        implicit_blocks = sum(g['allocated_count'] for g in implicit if g['kind'] == 'blocks')
        assert u32(1028)-u32(1044)-len(allocated['blocks'])-implicit_blocks == u32(1036)
        assert u32(1024)-len(allocated['inodes']) == u32(1040)
        assert all(role['inode'] in allocated['inodes'] for role in roles.values())
        detail = {'name': name, 'image_sha256': sha(image), 'directory_roles': roles,
                  'slack_bytes_counts': free_lengths, 'publication_sized_slots': available,
                  'free_blocks': u32(1036), 'free_inodes': u32(1040), 'allocated': allocated,
                  'uninitialized_groups': implicit}
        (campaign/(name+'-allocation.json')).write_text(json.dumps(detail, indent=2)+'\n')
        results.append({k: v for k, v in detail.items() if k != 'allocated'})
        assert sha(image) == case['image_sha256']
    result = {'result': 'PASS', 'states': len(results), 'cases': results,
              'observer_sha256': sha(Path(__file__)),
              'limitation': 'bitmap/slot snapshots; not a proof that every allocation delta has exclusive account ownership'}
    (campaign/'namespace-observations.json').write_text(json.dumps(result, indent=2)+'\n')
    print('PASS read-only slots and bitmap counts:', len(results), 'states')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('campaign', type=Path)
    observe(parser.parse_args().campaign)
