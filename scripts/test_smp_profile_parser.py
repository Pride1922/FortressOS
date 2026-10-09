#!/usr/bin/env python3
"""Reject incomplete or inconsistent profile evidence; synthetic records only."""
import unittest
from test_smpbench_qemu import PROFILE_PHASES, validate_profile
from analyze_wait_profile import validate_pipe_profile


def fixture():
    lines = []
    for rep in range(2):
        lines += [f"smpbench_rep rep={rep} warmup={int(rep == 0)} setup_us=1 barrier_ok=1",
                  f"smpbench_barrier rep={rep} start_spread_us=0",
                  f"smpbench_parent rep={rep} profile_valid=1 hz=1000000 launch=1 ready=1 release=1 join=1 collect=1"]
        row = f"smpbench_worker rep={rep} worker_id=0 time_us_start=1000 time_us_end=2000 time_us=1000 profile=1 profile_valid=1 profile_hz=1000000 profile_cycles=1000 p_other=400"
        for phase in PROFILE_PHASES:
            row += f" p_{phase}={600 if phase == 'signal' else 0} p_{phase}_n={2 if phase == 'signal' else 0} p_{phase}_max={500 if phase == 'signal' else 0}"
        lines.append(row)
    lines.append("smpbench rev=3 profile=1 barrier=pipe metric=max_worker_us cpus_online=1")
    return "\n".join(lines)


class ProfileEvidence(unittest.TestCase):
    def test_pipe_transfers(self):
        row = {'iterations': '1'}
        values = {'diag': 1, 'valid': 1, 'reads': 3, 'writes': 3,
                  'read_bytes': 1136, 'write_bytes': 1136, 'read_max': 1024, 'write_max': 1024,
                  'read_small': 2, 'read_1k': 1, 'read_large': 0,
                  'write_small': 2, 'write_1k': 1, 'write_large': 0,
                  'read_waits': 1, 'write_waits': 0, 'empty_drains': 2, 'empty_fills': 2,
                  'full_fills': 0, 'reader_cpus': 1, 'writer_cpus': 2, 'direction_changes': 3, 'cpu_changes': 3}
        row.update({'pi_'+key: str(value) for key,value in values.items()})
        validate_pipe_profile([row], 'pipes', True)
        for key, value in (('valid', 0), ('write_bytes', 1135), ('read_1k', 2), ('reader_cpus', 0),
                           ('writer_cpus', 1 << 64), ('empty_drains', 4), ('full_fills', 4),
                           ('direction_changes', 6), ('cpu_changes', 6), ('read_waits', -1)):
            with self.subTest(key=key), self.assertRaises(AssertionError):
                validate_pipe_profile([dict(row, **{'pi_'+key: str(value)})], 'pipes', True)
        with self.assertRaises(AssertionError):
            validate_pipe_profile([row, {'iterations': '1'}], 'pipes', True)
        with self.assertRaises(AssertionError):
            validate_pipe_profile([row], 'pipes', False)

    def test_scheduler_waits(self):
        trace = ' wait_diag=1 wait_valid=1 wait_blocks=2 wait_wakes=2 wait_selections=2 wait_resumes=2 wait_blocked=100 wait_ready=200 wait_resume=50 wait_ready_max=150 wait_hz=1000000 wait_total=1000'
        text = fixture().replace('worker_id=0', 'worker_id=0' + trace)
        validate_profile(text, 1, 1)
        for before, after in (('wait_valid=1','wait_valid=0'), ('wait_wakes=2','wait_wakes=1'),
                              ('wait_ready_max=150','wait_ready_max=201'),
                              ('wait_blocked=100','wait_blocked=900'), ('wait_hz=1000000','wait_hz=0')):
            with self.subTest(after=after), self.assertRaises(AssertionError):
                validate_profile(text.replace(before, after, 1), 1, 1)

    def test_valid(self):
        validate_profile(fixture(), 1, 1)

    def test_reject_corruption(self):
        for before, after in (("p_other=400", "p_other=401"),
                              ("p_signal_max=500", "p_signal_max=700"),
                              ("p_signal_n=2", "p_signal_n=0"),
                              ("profile_valid=1", "profile_valid=0"),
                              ("time_us=1000", "time_us=900"),
                              ("time_us_end=2000", "time_us_end=999")):
            with self.subTest(after=after), self.assertRaises(AssertionError):
                validate_profile(fixture().replace(before, after, 1), 1, 1)

    def test_missing_parent(self):
        with self.assertRaises(AssertionError):
            validate_profile("\n".join(line for line in fixture().splitlines()
                                       if not line.startswith("smpbench_parent rep=1")), 1, 1)

    def test_spawn_partition(self):
        text = fixture().replace("p_signal=600", "p_signal=0").replace("p_signal_n=2", "p_signal_n=0").replace("p_signal_max=500", "p_signal_max=0")
        text = text.replace("p_spawn=0", "p_spawn=600").replace("p_spawn_n=0", "p_spawn_n=2").replace("p_spawn_max=0", "p_spawn_max=500")
        detail = " spawn_diag=1 sp_valid=1 sp_total=500 sp_calls=2 sp_failures=0 sp_other=50"
        for name in ("file", "reap", "elf", "ustack", "kstack", "tcb", "fds", "publish", "cleanup"):
            detail += f" sp_{name}={450 if name == 'elf' else 0}"
        for name in ("space", "alloc", "map", "copy"):
            detail += f" se_{name}={100 if name == 'map' else 0}"
        text = text.replace("worker_id=0", "worker_id=0" + detail)
        validate_profile(text, 1, 1)
        for before, after in (("sp_valid=1", "sp_valid=0"), ("sp_other=50", "sp_other=51"),
                              ("sp_calls=2", "sp_calls=1"), ("sp_failures=0", "sp_failures=1"),
                              ("se_map=100", "se_map=451"), ("sp_total=500", "sp_total=601")):
            with self.subTest(after=after), self.assertRaises(AssertionError):
                validate_profile(text.replace(before, after, 1), 1, 1)

    def test_writer_profile(self):
        text = fixture().replace("p_signal=600", "p_signal=0").replace("p_signal_n=2", "p_signal_n=0").replace("p_signal_max=500", "p_signal_max=0")
        text = text.replace("p_read=0", "p_read=600").replace("p_read_n=0", "p_read_n=2").replace("p_read_max=0", "p_read_max=500")
        text = text.replace("worker_id=0", "worker_id=0 iterations=2 writer_cycles=1000 writer_write=900 writer_write_n=2 writer_write_max=600 writer_compute=60 writer_compute_n=2 writer_compute_max=30 writer_other=40 writer_valid=1")
        validate_profile(text, 1, 1)
        for before, after in (("writer_valid=1", "writer_valid=0"),
                              ("writer_other=40", "writer_other=41"),
                              ("writer_write_max=600", "writer_write_max=901"),
                              ("writer_compute_n=2", "writer_compute_n=3")):
            with self.subTest(after=after), self.assertRaises(AssertionError):
                validate_profile(text.replace(before, after, 1), 1, 1)


if __name__ == "__main__":
    unittest.main()
