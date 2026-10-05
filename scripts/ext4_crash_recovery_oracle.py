"""Independent journal CRC/record preview and trace-bound metadata truth.

This oracle never calls FortressOS. A rejection needs a named on-media witness;
valid inputs must recover to the one recorded durable transaction prefix.
"""
import functools
import struct
from test_jbd2_replay_host import crc


@functools.lru_cache(maxsize=8192)
def checksum(seed, data):
    return crc(seed, data)


def be(data, offset): return struct.unpack_from('>I', data, offset)[0]


class Corruption(Exception): pass


def sealed(data, offset, seed):
    raw = bytearray(data); stored = be(raw, offset); raw[offset:offset+4] = bytes(4)
    return stored == checksum(seed, bytes(raw))


class Truth:
    def __init__(self, base, sector, block, events, payload, journal_map):
        self.base, self.ss, self.bs = base, sector, block
        self.journal_map = journal_map
        self.uuid = base[1128:1144]
        self.seed = checksum(0xffffffff, self.uuid)
        self.transactions = []
        for sequence in dict.fromkeys(e['sequence'] for e in events):
            writes = [e for e in events if e['sequence'] == sequence and e['kind'] == 'write']
            metadata_blocks = {p['block'] for e in writes for p in e['parts'] if p['role'] == 'metadata'}
            commits = {p['block'] for e in writes for p in e['parts'] if p['journal_type'] == 2}
            if not commits: continue
            assert len(commits) == 1
            commit = next(iter(commits)); images = {}
            for e in writes:
                for part in e['parts']:
                    b = part['block']
                    if b == commit or (e['state'] == 4 and b in metadata_blocks):
                        image = images.setdefault(b, bytearray(base[b*block:(b+1)*block]))
                        start = e['payload_offset'] + part['offset']; skip = part['skip']; size = part['length']
                        image[skip:skip+size] = payload[start:start+size]
            self.transactions.append({'sequence': sequence, 'commit': commit, 'commit_bytes': bytes(images.pop(commit)),
                                      'images': {b: bytes(v) for b, v in images.items()}})
        self.metadata_blocks = {b for tx in self.transactions for b in tx['images']}

    def preview(self, disk):
        mapping, bs = self.journal_map, self.bs
        journal = lambda n: disk.read_stable(mapping[n]*bs, bs)
        superblock = journal(0)
        if not sealed(superblock[:1024], 252, 0xffffffff): raise Corruption('journal-superblock-crc')
        if (be(superblock, 0), be(superblock, 4), be(superblock, 12), be(superblock, 16), be(superblock, 40)) != \
                (0xc03b3998, 4, bs, len(mapping), 0x11): raise Corruption('journal-superblock-identity')
        if superblock[48:64] != self.uuid: raise Corruption('journal-uuid')
        first, cursor, sequence = be(superblock, 20), be(superblock, 28), be(superblock, 24)
        if not 0 < first < len(mapping) or cursor and not first <= cursor < len(mapping): raise Corruption('journal-range')
        pending, images, revokes, committed_revokes = {}, {}, set(), set()
        consumed = 0
        while cursor and consumed < len(mapping)-first:
            record = journal(cursor);cursor = first if cursor+1 == len(mapping) else cursor+1;consumed += 1
            if be(record, 0) != 0xc03b3998: break
            if be(record, 8) != sequence:
                if (be(record, 8)-sequence) & 0xffffffff < 0x80000000: raise Corruption('journal-future-sequence')
                break
            kind = be(record, 4)
            if kind == 1:
                if not sealed(record, bs-4, self.seed): raise Corruption('descriptor-crc')
                at = 12
                while True:
                    if at+16 > bs-4: raise Corruption('descriptor-bounds')
                    home, flags, reserved, stored = struct.unpack_from('>IIII', record, at);at += 16
                    if flags & ~11 or reserved or home in mapping or home >= len(self.base)//bs:
                        raise Corruption('descriptor-target-flags')
                    if not flags & 2:
                        if record[at:at+16] != self.uuid: raise Corruption('descriptor-uuid')
                        at += 16
                    data = journal(cursor);cursor = first if cursor+1 == len(mapping) else cursor+1;consumed += 1
                    if checksum(checksum(self.seed, struct.pack('>I', sequence)), data) != stored:
                        raise Corruption('journal-payload-crc')
                    if flags & 1:
                        if be(data, 0): raise Corruption('escape-payload')
                        data = struct.pack('>I', 0xc03b3998) + data[4:]
                    pending[home] = data
                    if flags & 8: break
            elif kind == 5:
                if not sealed(record, bs-4, self.seed): raise Corruption('revoke-crc')
                end = be(record, 12)
                if end < 16 or end > bs-4 or (end-16)%4: raise Corruption('revoke-bounds')
                revokes.update(be(record, at) for at in range(16, end, 4))
            elif kind == 2:
                if record[12:14] != bytes(2) or not sealed(record, 16, self.seed): raise Corruption('commit-crc')
                images.update(pending);committed_revokes.update(revokes)
                # Later reuse cancels older revoke suppression for new images.
                committed_revokes.difference_update(pending.keys()-revokes)
                pending, revokes = {}, set();sequence = (sequence+1) & 0xffffffff
            else: raise Corruption('journal-record-type')
        return {b: v for b, v in images.items() if b not in committed_revokes}

    def expected(self, disk):
        try:
            preview = self.preview(disk)
            last = -1
            for index, tx in enumerate(self.transactions):
                if disk.read_stable(tx['commit']*self.bs, self.bs) == tx['commit_bytes']: last = index
            expected = {b: self.base[b*self.bs:(b+1)*self.bs] for b in self.metadata_blocks}
            for tx in self.transactions[:last+1]: expected.update(tx['images'])
            for b, value in expected.items():
                observed = preview.get(b, disk.read_stable(b*self.bs, self.bs))
                if observed != value: raise Corruption(f'nonjournaled-metadata-mismatch-block-{b}')
            sb_block = 1024//self.bs
            sb = preview.get(sb_block, disk.read_stable(sb_block*self.bs, self.bs))
            at = 1024%self.bs; sb = sb[at:at+1024]
            if struct.unpack_from('<I', sb, 1020)[0] != checksum(0xffffffff, sb[:1020]):
                raise Corruption('preview-filesystem-superblock-crc')
            return int(last >= 0), 'durable-prefix'
        except Corruption as error:
            return 2, str(error)
