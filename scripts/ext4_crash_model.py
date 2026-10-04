"""Independent host persistence model. No kernel code or physical-device claim.

Each scheduled sector persists its latest accepted version. Reordering schedules
select sectors, not superseded historical versions. Torn writes are explicit.
"""
from dataclasses import dataclass


class Disconnected(IOError):
    pass


@dataclass(frozen=True)
class Fault:
    event: int
    after: bool = False
    tear_bytes: int = 0
    flush_sectors: tuple[int, ...] = ()

    def __post_init__(self):
        if not isinstance(self.event, int) or self.event < 0 or self.tear_bytes < 0:
            raise ValueError('fault configuration')


class Disk:
    def __init__(self, initial: bytes, sector: int, *, write_through=False,
                 early_schedule=None, fault=None, false_flush=False):
        if sector not in (512, 4096) or not initial or len(initial) % sector:
            raise ValueError('geometry')
        self.stable = bytearray(initial)
        self.volatile = bytearray(initial)
        self.sector = sector
        self.dirty = set()
        self.event = 0
        self.offline = False
        self.write_through = write_through
        self.early_schedule = early_schedule or {}
        self.fault = fault
        self.false_flush = false_flush  # Negative control only.
        if fault:
            if fault.tear_bytes >= sector:
                raise ValueError('tear size')
            for lba in fault.flush_sectors: self._range(lba)
        for event, sectors in self.early_schedule.items():
            if not isinstance(event, int) or event < 0:
                raise ValueError('schedule event')
            for lba in sectors: self._range(lba)

    def _range(self, lba):
        if not isinstance(lba, int) or lba < 0 or lba >= len(self.stable) // self.sector:
            raise ValueError('sector range')
        return slice(lba * self.sector, (lba + 1) * self.sector)

    def persist(self, sectors):
        if self.offline:
            raise Disconnected('offline')
        # Validate the whole schedule before changing stable media.
        ranges = [(lba, self._range(lba)) for lba in sectors]
        for lba, span in ranges:
            self.stable[span] = self.volatile[span]
            self.dirty.discard(lba)

    def _begin(self):
        if self.offline:
            raise Disconnected('offline')
        fault = self.fault if self.fault and self.fault.event == self.event else None
        self.event += 1
        return fault

    def _disconnect(self):
        self.offline = True
        raise Disconnected('uncertain I/O')

    def write(self, lba, data):
        span = self._range(lba)
        if len(data) != self.sector:
            raise ValueError('sector payload')
        fault = self._begin()
        if fault and fault.flush_sectors:
            raise ValueError('flush schedule on write')
        if fault and fault.tear_bytes:
            if not 0 < fault.tear_bytes < self.sector:
                raise ValueError('tear size')
            start = lba * self.sector
            self.stable[start:start + fault.tear_bytes] = data[:fault.tear_bytes]
            self._disconnect()
        if fault and not fault.after:
            self._disconnect()
        self.volatile[span] = data
        self.dirty.add(lba)
        if self.write_through:
            self.persist((lba,))
        self.persist(self.early_schedule.get(self.event - 1, ()))
        if fault:
            self._disconnect()

    def flush(self):
        fault = self._begin()
        if fault and fault.tear_bytes:
            raise ValueError('sector tear on flush')
        if fault and not fault.after:
            self.persist(fault.flush_sectors)
            self._disconnect()
        if not self.false_flush:
            self.persist(sorted(self.dirty))
        if fault:
            self._disconnect()

    def read(self, lba):
        span = self._range(lba)
        if self.offline:
            raise Disconnected('offline')
        return bytes(self.volatile[span])

    def restart(self):
        self.volatile[:] = self.stable
        self.dirty.clear()
        self.offline = False
        self.event = 0
        self.fault = None
        self.early_schedule = {}


def replay(initial, sector, events, payload, **settings):
    disk = Disk(initial, sector, **settings)
    expected_offset = 0
    for index, event in enumerate(events):
        if event['index'] != index or event['payload_offset'] != expected_offset:
            raise ValueError('noncontiguous trace')
        if event['kind'] == 'write':
            data = payload[expected_offset:expected_offset + sector]
            disk.write(event['lba'], data)
            expected_offset += sector
        elif event['kind'] == 'flush':
            disk.flush()
        else:
            raise ValueError('unknown event')
    if expected_offset != len(payload):
        raise ValueError('unreferenced payload')
    return disk


def validate_snapshot(observed, allowed):
    """Allowed complete states; never mix fields from different transaction states.

    Snapshots contain independently observed namespace/data/ownership/link/orphan
    facts. Exact comparisons also reject stale bytes and allocation aliasing.
    """
    if observed not in allowed:
        raise AssertionError('snapshot outside declared transaction boundaries')


def audit_barriers(initial, sector, block_size, events, payload, *, omit_flush=None):
    """Check physical dependencies in recorded actual writer operations.

    Uses byte equality on stable media, not writer state as proof of durability.
    omit_flush is a negative control using the original event/payload indices.
    """
    disk = Disk(initial, sector)
    sequence = None
    ordered, journal, metadata = {}, {}, {}
    commit = None

    def durable(images, except_block=None):
        for (start, end), (block, value) in images.items():
            if block != except_block and disk.stable[start:end] != value:
                raise AssertionError('missing durability dependency')

    for event in events:
        if sequence != event['sequence']:
            sequence = event['sequence']
            ordered, journal, metadata = {}, {}, {}
            commit = None
        if event['kind'] == 'flush':
            if event['index'] != omit_flush:
                disk.flush()
            continue
        offset = event['payload_offset']
        value = payload[offset:offset + sector]
        for part in event['parts']:
            block, role = part['block'], part['role']
            start = event['lba'] * sector + part['offset']
            end = start + part['length']
            if part['journal_type'] == 2:
                durable(ordered)
                durable(journal, except_block=block)
                commit = block
            if role == 'metadata':
                if commit is None:
                    raise AssertionError('home metadata before commit record')
                span = slice(commit * block_size, (commit + 1) * block_size)
                if disk.stable[span] != disk.volatile[span]:
                    raise AssertionError('home metadata before durable commit')
            if role == 'journal' and part['journal_type'] == 4 and event['state'] == 4:
                durable(metadata)
            images = ordered if role == 'ordered' else metadata if role == 'metadata' else journal if role == 'journal' else None
            if images is not None:
                begin = part['offset']
                images[(start, end)] = (block, value[begin:begin + part['length']])
        disk.write(event['lba'], value)
    return disk
