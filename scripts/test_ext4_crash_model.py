"""Calibration and negative controls for the Phase-9 host persistence model."""
import unittest
from ext4_crash_model import Disk, Disconnected, Fault, replay, validate_snapshot


class Calibration(unittest.TestCase):
    def disks(self):
        for sector in (512, 4096):
            yield sector, Disk(bytes(4 * sector), sector)

    def test_cache_loss_and_flush(self):
        for ss, d in self.disks():
            d.write(1, b'A' * ss)
            self.assertEqual(d.read(1), b'A' * ss)
            self.assertEqual(d.stable, bytes(4 * ss))
            d.restart()
            self.assertEqual(d.read(1), bytes(ss))
            d.write(1, b'B' * ss); d.flush(); d.restart()
            self.assertEqual(d.read(1), b'B' * ss)

    def test_write_through(self):
        for ss, _ in self.disks():
            d = Disk(bytes(4 * ss), ss, write_through=True)
            d.write(2, b'W' * ss); d.restart()
            self.assertEqual(d.read(2), b'W' * ss)

    def test_reorder_and_latest_version(self):
        for ss, _ in self.disks():
            d = Disk(bytes(4 * ss), ss, early_schedule={2: (2, 0)})
            d.write(0, b'A' * ss); d.write(1, b'B' * ss); d.write(2, b'C' * ss)
            self.assertEqual(d.stable, b'A' * ss + bytes(ss) + b'C' * ss + bytes(ss))
            d.write(0, b'D' * ss); d.flush(); d.restart()
            self.assertEqual(d.read(0), b'D' * ss)
            self.assertEqual(d.read(1), b'B' * ss)

    def test_every_atomic_boundary(self):
        for ss, _ in self.disks():
            for wt in (False, True):
                for cut in range(3):
                    for after in (False, True):
                        d = Disk(bytes(4 * ss), ss, write_through=wt, fault=Fault(cut, after))
                        actions = (lambda: d.write(0, b'A' * ss), lambda: d.write(1, b'B' * ss), d.flush)
                        with self.assertRaises(Disconnected):
                            for action in actions: action()
                        expected = bytearray(4 * ss)
                        accepted = cut + int(after)
                        if wt or (cut == 2 and after):
                            if accepted >= 1: expected[:ss] = b'A' * ss
                            if accepted >= 2: expected[ss:2 * ss] = b'B' * ss
                        self.assertEqual(d.stable, expected)
                        event = d.event
                        with self.assertRaises(Disconnected): d.flush()
                        self.assertEqual(d.event, event)
                        d.restart(); self.assertEqual(d.volatile, expected)

    def test_partial_failed_flush(self):
        for ss, _ in self.disks():
            for schedule in ((), (0,), (1,), (1, 0)):
                d = Disk(bytes(4 * ss), ss, fault=Fault(2, flush_sectors=schedule))
                d.write(0, b'A' * ss); d.write(1, b'B' * ss)
                with self.assertRaises(Disconnected): d.flush()
                d.restart()
                self.assertEqual(d.read(0), (b'A' if 0 in schedule else b'\0') * ss)
                self.assertEqual(d.read(1), (b'B' if 1 in schedule else b'\0') * ss)

    def test_every_sector_prefix_tear(self):
        for ss, _ in self.disks():
            for prefix in range(1, ss):
                d = Disk(bytes(4 * ss), ss, fault=Fault(0, tear_bytes=prefix))
                with self.assertRaises(Disconnected): d.write(1, b'T' * ss)
                self.assertEqual(d.stable, bytes(ss) + b'T' * prefix + bytes(3 * ss - prefix))

    def test_false_flush_and_missing_ordering_detected(self):
        for ss, _ in self.disks():
            # Synthetic ordering invariant: stable reference must have stable data.
            for broken in ('omit-data-flush', 'lying-flush'):
                d = Disk(bytes(4 * ss), ss, false_flush=broken == 'lying-flush')
                d.write(0, b'D' * ss)
                if broken != 'omit-data-flush': d.flush()
                d.write(1, b'R' * ss); d.persist((1,)); d.restart()
                observed = {'reference': d.read(1), 'data': d.read(0)}
                with self.assertRaises(AssertionError):
                    validate_snapshot(observed, [{'reference': b'R' * ss, 'data': b'D' * ss}])

    def test_snapshot_negative_controls(self):
        before = {'names': {'old': 11}, 'bytes': b'old', 'owners': {7: 11}, 'links': {11: 1}, 'orphans': ()}
        after = {'names': {'new': 11}, 'bytes': b'new', 'owners': {8: 11}, 'links': {11: 1}, 'orphans': ()}
        validate_snapshot(before, [before, after]); validate_snapshot(after, [before, after])
        for key, bad in (('names', {'old': 11, 'new': 11}), ('bytes', b'stale'),
                         ('owners', {7: (11, 12)}), ('links', {11: 2}), ('orphans', (11,))):
            observed = dict(after); observed[key] = bad
            with self.assertRaises(AssertionError): validate_snapshot(observed, [before, after])

    def test_bounds_and_trace_corruption(self):
        for ss, d in self.disks():
            for lba in (-1, 4, 2**64):
                with self.assertRaises(ValueError): d.write(lba, bytes(ss))
            with self.assertRaises(ValueError): d.write(0, bytes(ss - 1))
            self.assertEqual(d.event, 0)
            with self.assertRaises(ValueError): d.persist((0, 4))
            self.assertEqual(d.stable, bytes(4 * ss))
            with self.assertRaises(ValueError): replay(bytes(4 * ss), ss, [{'index': 1, 'kind': 'flush', 'payload_offset': 0}], b'')
            with self.assertRaises(ValueError): replay(bytes(4 * ss), ss, [], b'extra')


if __name__ == '__main__':
    unittest.main(verbosity=2)
