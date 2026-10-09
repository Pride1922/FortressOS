#!/usr/bin/env python3
"""Validate and attribute opt-in worker profiles; elapsed time is not CPU time."""
import argparse
import json
import re
from pathlib import Path
from test_smpbench_qemu import PROFILE_PHASES, validate_profile


def fields(line):
    return dict(re.findall(r"(\w+)=([^ \n]+)", line))


def analyze(path):
    text = re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", path.read_text(errors="replace")).replace("\r", "")
    records, section = [], []
    for line in text.splitlines():
        section.append(line)
        if not line.startswith("w="):
            continue
        summary = fields(line)
        if "smpbench rev=3 profile=1 " not in "\n".join(section):
            section = []
            continue
        assert summary["ok"] == "1" and summary["short"] == "0", "Failed workload"
        validate_profile("\n".join(section), int(summary["n"]), int(summary["reps"]))
        workers = [fields(row) for row in section if row.startswith("smpbench_worker ")]
        parents = {int(row["rep"]): row for row in map(fields, section)
                   if "launch" in row and "join" in row and "rep" in row}
        reps = []
        for rep in range(1, int(summary["reps"]) + 1):
            cohort = [row for row in workers if int(row["rep"]) == rep]
            critical = max(cohort, key=lambda row: int(row["profile_cycles"]))
            hz = int(critical["profile_hz"])
            total = int(critical["profile_cycles"])
            phases = {}
            for phase in (*PROFILE_PHASES, "other"):
                cycles = int(critical["p_" + phase])
                phases[phase] = {"cycles": cycles, "us": cycles * 1000000 / hz,
                                 "percent": 100 * cycles / max(total, 1)}
                if phase != "other":
                    phases[phase].update({"calls": int(critical["p_" + phase + "_n"]),
                        "max_call_us": int(critical["p_" + phase + "_max"]) * 1000000 / hz})
            parent = parents[rep]
            writer = None
            if "writer_cycles" in critical:
                writer = {name: int(critical["writer_" + name]) * 1000000 / hz
                          for name in ("cycles", "write", "write_max", "compute", "compute_max", "other")}
                writer.update({name: int(critical["writer_" + name]) for name in ("write_n", "compute_n")})
                if "writer_startup_valid" in critical:
                    writer.update({name + "_us": int(critical["writer_" + name]) * 1000000 / hz
                                   for name in ("queued_to_run", "run_to_entry")})
            spawn = None
            if "spawn_diag" in critical:
                spawn = {"total_us": int(critical["sp_total"]) * 1000000 / hz,
                         "calls": int(critical["sp_calls"]), "failures": int(critical["sp_failures"]),
                         "phases_us": {name: int(critical["sp_" + name]) * 1000000 / hz
                             for name in ("file", "reap", "elf", "ustack", "kstack", "tcb", "fds", "publish", "cleanup", "other")},
                         "elf_subphases_us": {name: int(critical["se_" + name]) * 1000000 / hz
                             for name in ("space", "alloc", "map", "copy")}}
            reps.append({"rep": rep, "critical_worker": int(critical["worker_id"]),
                         "worker_us": total * 1000000 / hz,
                         "cpu_sample_valid": critical.get("cpu_valid") == "1",
                         "cpu_sample_us": int(critical.get("cpu_ticks", 0)) * 1000000 / int(critical.get("tick_hz", 100)),
                         "dominant": max(phases, key=lambda phase: phases[phase]["cycles"]),
                         "phases": phases,
                         "writer": writer,
                         "spawn": spawn,
                         "scheduler_wait": {"blocks": int(critical['wait_blocks']),
                             **{name + '_us': int(critical['wait_' + name]) * 1000000 / int(critical['wait_hz'])
                                for name in ('blocked', 'ready', 'resume', 'ready_max')}} if 'wait_diag' in critical else None,
                         "parent_us": {name: int(parent[name]) * 1000000 / int(parent["hz"])
                                       for name in ("launch", "ready", "release", "join", "collect")},
                         "workers": cohort})
        records.append({"log": str(path.resolve()), "workload": summary["w"], "summary": summary,
                        "repetitions": reps})
        section = []
    assert records, f"No complete profile workloads in {path}"
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    assert not args.output.exists(), "Choose a new evidence output"
    records = [record for path in args.logs for record in analyze(path)]
    args.output.write_text(json.dumps({"scope": "Elapsed attribution, including scheduler/host delay; warmup excluded",
                                      "records": records}, indent=2))
    for record in records:
        reps = sorted(record["repetitions"], key=lambda row: row["worker_us"])
        for label, row in (("fastest", reps[0]), ("slowest", reps[-1])):
            phase = row["phases"][row["dominant"]]
            print(f"{Path(record['log']).parent.parent.name} {record['workload']} {label} "
                  f"rep={row['rep']} worker={row['critical_worker']} elapsed_us={row['worker_us']:.0f} "
                  f"phase={row['dominant']} share={phase['percent']:.1f}% "
                  f"max_call_us={phase.get('max_call_us', 0):.0f} cpu_sample_us={row['cpu_sample_us']:.0f}")


if __name__ == "__main__":
    main()
