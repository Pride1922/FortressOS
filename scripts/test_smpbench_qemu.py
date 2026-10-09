#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
import re
import socket
import subprocess
import tempfile
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PROMPT_PATTERN = re.compile(r'(?:fortress> |(?:\[\d+\] )?fortress:[^\r\n]* \$ )')

def validate_barrier(text, workers, reps):
    assert re.search(r"^smpbench (?:rev=2 |rev=3 profile=1 )barrier=pipe metric=max_worker_us ", text, re.M), "Missing barrier revision"
    def rows(prefix):
        return [dict(re.findall(r"(\w+)=([^ \n]+)", line))
                for line in text.splitlines() if line.startswith(prefix + " ")]
    repetitions, spreads, results = rows("smpbench_rep"), rows("smpbench_barrier"), rows("smpbench_worker")
    assert len(repetitions) == len(spreads) == reps + 1, "Missing repetition/barrier evidence"
    assert len(results) == workers * (reps + 1), "Missing worker results"
    for rep in range(reps + 1):
        cohort = [row for row in results if int(row["rep"]) == rep]
        assert sorted(int(row["worker_id"]) for row in cohort) == list(range(workers)), "Duplicate/missing worker"
        row = repetitions[rep]
        assert int(row["rep"]) == rep and row["barrier_ok"] == "1", "Readiness barrier failed"
        assert int(row["setup_us"]) >= 0
        assert int(row["warmup"]) == (rep == 0)
        starts = [int(row["time_us_start"]) for row in cohort]
        assert all(int(row["time_us_end"]) >= int(row["time_us_start"]) for row in cohort), "Reversed worker clock"
        spread = spreads[rep]
        assert int(spread["rep"]) == rep
        assert int(spread["start_spread_us"]) == max(starts) - min(starts), "Incorrect start spread"
    for row in results:
        if "wait_diag" not in row:
            continue
        assert row["wait_diag"] == row["wait_valid"] == "1", "Invalid scheduler wait trace"
        counts = [int(row['wait_' + name]) for name in ('blocks', 'wakes', 'selections', 'resumes')]
        assert len(set(counts)) == 1 and counts[0] >= 0, "Incomplete scheduler wait trace"
        intervals = [int(row['wait_' + name]) for name in ('blocked', 'ready', 'resume')]
        assert all(value >= 0 for value in intervals)
        assert 0 <= int(row['wait_ready_max']) <= intervals[1]
        assert counts[0] or (sum(intervals) == int(row['wait_ready_max']) == 0)
        assert sum(intervals) <= int(row['wait_total']), "Wait intervals exceed worker"
        assert int(row['wait_hz']) > 0
        assert int(row['time_us']) == int(row['wait_total']) * 1000000 // int(row['wait_hz'])

PROFILE_PHASES = ("setup", "spawn", "wait", "signal", "header", "read", "compute", "close", "write")

def validate_profile(text, workers, reps):
    validate_barrier(text, workers, reps)
    assert "smpbench rev=3 profile=1 " in text, "Missing profile revision"
    results = [dict(re.findall(r"(\w+)=([^ \n]+)", line))
               for line in text.splitlines() if line.startswith("smpbench_worker ")]
    for row in results:
        assert row["profile"] == row["profile_valid"] == "1", "Invalid phase clock/accounting"
        assert int(row["profile_hz"]) > 0
        total = int(row["p_other"])
        for phase in PROFILE_PHASES:
            cycles, calls, maximum = (int(row["p_" + phase + suffix]) for suffix in ("", "_n", "_max"))
            assert 0 <= maximum <= cycles
            assert (calls == 0) == (cycles == maximum == 0), "Missing/inconsistent phase calls"
            total += cycles
        assert total == int(row["profile_cycles"]), "Phase partition does not cover worker duration"
        assert int(row["time_us"]) == total * 1000000 // int(row["profile_hz"]), "Phase/worker time mismatch"
        if "spawn_diag" in row:
            assert row["spawn_diag"] == row["sp_valid"] == "1"
            assert int(row["sp_calls"]) == int(row["p_spawn_n"])
            assert int(row["sp_failures"]) == 0
            assert int(row["sp_total"]) <= int(row["p_spawn"])
            assert int(row["sp_total"]) == sum(int(row["sp_" + phase]) for phase in
                ("file", "reap", "elf", "ustack", "kstack", "tcb", "fds", "publish", "cleanup", "other"))
            assert sum(int(row["se_" + phase]) for phase in ("space", "alloc", "map", "copy")) <= int(row["sp_elf"])
        if "writer_cycles" in row:
            assert row["writer_valid"] == "1"
            assert int(row["writer_cycles"]) == sum(int(row[key]) for key in ("writer_write", "writer_compute", "writer_other"))
            for phase in ("write", "compute"):
                cycles, calls, maximum = (int(row["writer_" + phase + suffix]) for suffix in ("", "_n", "_max"))
                assert 0 <= maximum <= cycles and calls > 0
            batch = int(row.get('pipe_batch', '1'))
            assert batch in (1,4)
            assert int(row["writer_compute_n"]) == (int(row["iterations"])+batch-1)//batch
            if "spawn_diag" in row:
                assert row["writer_startup_valid"] == "1", "Missing/invalid writer birth stamps"
                assert int(row["writer_queued_to_run"]) >= 0 and int(row["writer_run_to_entry"]) >= 0
        elif int(row["p_read_n"]) > 0:
            raise AssertionError("Missing pipe writer profile")
    parents = [dict(re.findall(r"(\w+)=([^ \n]+)", line))
               for line in text.splitlines() if line.startswith("smpbench_parent ")]
    assert len(parents) == reps + 1
    assert sorted(int(row["rep"]) for row in parents) == list(range(reps + 1))
    assert all(row["profile_valid"] == "1" and int(row["hz"]) > 0 for row in parents)
    assert all(int(row[name]) >= 0 for row in parents for name in ("launch", "ready", "release", "join", "collect"))

def parse_snapshot(text):
    locks = {}
    for line in text.splitlines():
        if line.startswith("lockstat name="):
            fields = dict(re.findall(r"(\w+)=([^ ]+)", line))
            locks[fields["name"]] = {k: int(fields[k]) for k in ("acquires", "contentions", "max_spin")}
    match = re.search(r"\[TLBSTAT\] ([^\n]+)", text)
    assert locks and match, "Missing lock/TLB statistics"
    return {"locks": locks, "tlb": {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", match[1])}}


def run_smpbench_test(smp_count, iso, label, run, reps, checkpoint=None, allow_missing_cpu=False):
    print(f"\n========================================================")
    print(f"Testing SMP={smp_count} QEMU session")
    print(f"========================================================")
    with tempfile.TemporaryDirectory(prefix=f"fortress-smpbench-{smp_count}-") as tmp:
        uart_path = Path(tmp) / "uart.sock"
        log = REPO / "build" / f"smpbench-{label}-smp{smp_count}-run{run}.log"
        assert not log.exists(), f"Evidence already exists: {log}; choose a new --label"

        cmd = [
            "qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", "2G",
            "-smp", str(smp_count), "-display", "none", "-no-reboot", "-monitor", "none",
            "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log}",
            "-serial", "chardev:uart",
            "-boot", "d", "-cdrom", str(iso)
        ]

        assert not any(arg in cmd for arg in ("-drive", "-blockdev", "-hda", "-hdb", "-device"))
        records = []
        sock = None
        proc = subprocess.Popen(cmd, cwd=REPO)
        stop_drain = threading.Event()
        try:
            # Wait for socket
            sock = None
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if uart_path.exists():
                    try:
                        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                        sock.connect(str(uart_path))
                        break
                    except (ConnectionRefusedError, FileNotFoundError):
                        time.sleep(0.1)
                time.sleep(0.1)
            assert sock is not None, "Failed to connect to UART socket"

            # Drain UART continuously in background
            sock.settimeout(0.2)
            def drain():
                while not stop_drain.is_set():
                    try:
                        if not sock.recv(65536):
                            return
                    except socket.timeout:
                        continue
                    except Exception:
                        return
            drain_thread = threading.Thread(target=drain, daemon=True)
            drain_thread.start()

            def output():
                if not log.exists():
                    return ""
                return re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", log.read_bytes().decode(errors="replace")).replace("\r", "")

            # Wait for shell prompt
            deadline = time.monotonic() + 45
            prompt_seen = False
            while time.monotonic() < deadline:
                text = output()
                if PROMPT_PATTERN.search(text):
                    prompt_seen = True
                    break
                assert proc.poll() is None, "QEMU terminated unexpectedly"
                time.sleep(0.1)
            assert prompt_seen, f"Shell prompt not reached at SMP={smp_count}:\n{output()[-1000:]}"

            def send_cmd(command):
                start = len(output())
                for byte in (command + "\n").encode():
                    sock.send(bytes([byte]))
                    time.sleep(0.005)
                deadline = time.monotonic() + 90
                while time.monotonic() < deadline:
                    cur = output()[start:]
                    assert "KERNEL PANIC" not in cur and "[FATAL]" not in cur, cur[-1800:]
                    if PROMPT_PATTERN.search(cur):
                        time.sleep(0.1)
                        # Extract between command and next prompt
                        lines = cur.splitlines()
                        return "\n".join(lines[1:-1]) if len(lines) > 2 else cur
                    assert proc.poll() is None, "QEMU died during command"
                    time.sleep(0.1)
                raise TimeoutError(f"Command {command!r} timed out:\n{output()[-1000:]}")

            for workload in ("cpu_scale", "spawn_wait", "signals", "pipes"):
                before = parse_snapshot(send_cmd("lockstat -c"))
                result = send_cmd(f"smpbench -w {workload} -r {reps} -c")
                print(result.strip(), flush=True)
                summary = re.search(r"^w=" + workload + r" ([^\n]+)", result, re.M)
                assert summary, f"Missing benchmark summary: {result}"
                fields = dict(re.findall(r"(\w+)=([^ ]+)", summary[1]))
                assert fields["ok"] == "1" and fields["short"] == "0", result
                if "smpbench rev=3 " in result:
                    validate_profile(result, int(fields["n"]), reps)
                elif "smpbench rev=2 " in result:
                    validate_barrier(result, int(fields["n"]), reps)
                after = parse_snapshot(send_cmd("lockstat -c"))
                deltas = {name: {k: values[k] - before["locks"].get(name, {}).get(k, 0)
                                 for k in ("acquires", "contentions")}
                          for name, values in after["locks"].items()}
                tlb_delta = {k: after["tlb"][k] - before["tlb"][k] for k in after["tlb"]}
                workers = [dict(re.findall(r"(\w+)=([^ ]+)", line))
                           for line in result.splitlines() if line.startswith("smpbench_worker ")]
                assert workers, "Missing worker measurements"
                cpu_valid = sum(w.get("cpu_valid") == "1" for w in workers)
                if cpu_valid != len(workers):
                    print(f"CPU accounting unavailable in {len(workers)-cpu_valid}/{len(workers)} workers", flush=True)
                    assert allow_missing_cpu, "Missing CPU accounting"
                print(f"DELTA {workload} vmm={deltas.get('vmm')} pmm={deltas.get('pmm')} tlb={tlb_delta}", flush=True)
                records.append({"cpus": smp_count, "run": run, "workload": workload,
                                "summary": fields, "before": before, "after": after,
                                "lock_delta": deltas, "tlb_delta": tlb_delta,
                                "workers": workers, "output": result, "log": str(log)})
                if checkpoint:
                    checkpoint(records[-1])
            return records

        finally:
            stop_drain.set()
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
            if sock is not None:
                sock.close()

def main():
    parser = argparse.ArgumentParser(description="Per-workload lock/TLB deltas and elapsed/CPU tick evidence; BIOS, no data disks.")
    parser.add_argument("--iso", type=Path, default=REPO / "bin/fortress.iso")
    parser.add_argument("--cpus", nargs="+", type=int, choices=(1, 4, 8), default=[1, 4, 8])
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--reps", type=int, default=5)
    parser.add_argument("--label", default="current")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--allow-missing-cpu", action="store_true",
                        help="Retain throughput from older instrumented binaries with invalid CPU samples; report missing samples.")
    args = parser.parse_args()
    assert re.fullmatch(r"[a-zA-Z0-9_-]+", args.label), "Invalid log label"
    assert 1 <= args.runs <= 10 and 1 <= args.reps <= 31
    iso = args.iso.resolve()
    digest = hashlib.sha256(iso.read_bytes()).hexdigest()
    records = []
    def save(record=None, failure=None):
        if record is not None:
            records.append(record)
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps({"label": args.label, "iso_sha256": digest,
                                              "records": records, "failure": failure}, indent=2))
    try:
        for run in range(1, args.runs + 1):
            for cpus in args.cpus:
                run_smpbench_test(cpus, iso, args.label, run, args.reps, save, args.allow_missing_cpu)
    except Exception as exc:
        save(failure=str(exc))
        raise
    save()

if __name__ == "__main__":
    main()
