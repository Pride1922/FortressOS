"""Sector-overlay equivalent of the calibrated full-image model, for campaigns."""
import hashlib
import struct
from ext4_crash_model import Disconnected


class SparseDisk:
    def __init__(self, base, sector, **config):
        self.base, self.sector = base, sector
        self.stable, self.volatile = {}, {}
        self.dirty = set()
        self.config = config
        self.event = 0
        self.offline = False

    def read_stable(self, offset, size):
        result = bytearray()
        while size:
            lba, skip = divmod(offset, self.sector)
            count = min(size, self.sector - skip)
            result += self.stable.get(lba, self.base[lba*self.sector:(lba+1)*self.sector])[skip:skip+count]
            offset += count;size -= count
        return bytes(result)

    def persist(self, sectors):
        for lba in sectors:
            value = self.volatile.get(lba, self.base[lba*self.sector:(lba+1)*self.sector])
            if value == self.base[lba*self.sector:(lba+1)*self.sector]: self.stable.pop(lba, None)
            else: self.stable[lba] = value
            self.dirty.discard(lba)

    def apply(self, event, payload, fault=None):
        assert not self.offline and event['index'] == self.event
        self.event += 1
        if event['kind'] == 'write':
            lba = event['lba'];at = event['payload_offset'];value = payload[at:at+self.sector]
            assert len(value) == self.sector and 0 <= lba < len(self.base)//self.sector
            if fault and fault.tear_bytes:
                old = self.read_stable(lba*self.sector, self.sector)
                self.stable[lba] = value[:fault.tear_bytes] + old[fault.tear_bytes:]
            elif not fault or fault.after:
                self.volatile[lba] = value;self.dirty.add(lba)
                if self.config.get('write_through'): self.persist((lba,))
                self.persist(self.config.get('early_schedule', {}).get(event['index'], ()))
        elif event['kind'] == 'flush':
            self.persist(sorted(self.dirty) if not fault or fault.after else fault.flush_sectors)
        else: raise ValueError('event kind')
        if fault:
            self.offline = True
            raise Disconnected()

    def canonical(self):
        result = b''
        for lba, value in sorted(self.stable.items()):
            if value != self.base[lba*self.sector:(lba+1)*self.sector]:
                result += struct.pack('<I', lba) + value
        return result


def digest(delta):
    return hashlib.sha256(delta).hexdigest()
