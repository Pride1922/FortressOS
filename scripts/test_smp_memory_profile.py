#!/usr/bin/env python3
"""SMP memory investigation: measurement-only kernel-stack allocation & free profiling.

Disposable ISO-only QEMU TCG runs at 1, 4, and 8 CPUs:
1. 1 CPU with profiling enabled
2. 4 CPUs with profiling enabled
3. 8 CPUs with profiling enabled
4. Matching uninstrumented controls (smp_memory_test disabled) at 1, 4, and 8 CPUs
Verifies:
- Worker CPU stability: pinned worker threads execute strictly on assigned CPU.
- Clean start and done gates with coordinator synchronization.
- Elapsed TSC ticks reported per-CPU and in aggregate.
- Subphase allocation/free timing and lock contention observations.
- Exact post-profile baseline equality: heap bytes, allocated blocks, table frames,
  PMM free pages, deferred list drained, and exact byte-for-byte PMM bitmap equality.
- No production data disks attached; disposable ISO only.
- Machine-readable result.json and complete serial logs retained in unique directory.
"""
import datetime
import json
from pathlib import Path
import re
import shutil
import subprocess
import time
from test_smp_memory_boot import make_iso, CODE, VARS, REPO
from test_memory_ext4 import digest


def parse_kprof_log(text):
    data = {
        "cpu_telemetry": [],
        "subintervals": [],
        "ipi_overlay": [],
        "summary": {},
        "subinterval_sum": {},
        "control_summary": {}
    }
    for match in re.finditer(
        r"\[KPROF_CPU\] cpu=(\d+) ops=(\d+) alloc_tsc_ticks=(\d+) alloc_inner_tsc=(\d+) free_tsc_ticks=(\d+) free_inner_tsc=(\d+) avg_alloc_ticks=(\d+) avg_free_ticks=(\d+)",
        text,
    ):
        data["cpu_telemetry"].append({
            "cpu": int(match.group(1)),
            "ops": int(match.group(2)),
            "alloc_tsc_ticks": int(match.group(3)),
            "alloc_inner_tsc": int(match.group(4)),
            "free_tsc_ticks": int(match.group(5)),
            "free_inner_tsc": int(match.group(6)),
            "avg_alloc_ticks": int(match.group(7)),
            "avg_free_ticks": int(match.group(8)),
        })

    for match in re.finditer(
        r"\[KPROF_SUBINTERVAL\] cpu=(\d+) slot_alloc_wait=(\d+) slot_alloc_hold=(\d+) alloc_prep=(\d+) pmm_alloc=(\d+) map_prep=(\d+) "
        r"vmm_map_wait=(\d+) vmm_map_hold=(\d+) vmm_map_pt=(\d+) vmm_map_pre_lock=(\d+) vmm_map_post_prep=(\d+) "
        r"vmm_map_put_op_wait=(\d+) vmm_map_put_op_hold=(\d+) map_dispatch=(\d+) map_ack_poll=(\d+) map_service=(\d+) "
        r"alloc_tail=(\d+) unmap_wait=(\d+) unmap_hold=(\d+) unmap_pt=(\d+) unmap_pre_lock=(\d+) unmap_post_prep=(\d+) "
        r"unmap_put_op_wait=(\d+) unmap_put_op_hold=(\d+) unmap_dispatch=(\d+) unmap_ack_poll=(\d+) unmap_service=(\d+) "
        r"free_mid=(\d+) pmm_free=(\d+) free_tail=(\d+) slot_free_wait=(\d+) slot_free_hold=(\d+)",
        text,
    ):
        data["subintervals"].append({
            "cpu": int(match.group(1)),
            "slot_alloc_wait": int(match.group(2)),
            "slot_alloc_hold": int(match.group(3)),
            "alloc_prep": int(match.group(4)),
            "pmm_alloc": int(match.group(5)),
            "map_prep": int(match.group(6)),
            "vmm_map_wait": int(match.group(7)),
            "vmm_map_hold": int(match.group(8)),
            "vmm_map_pt": int(match.group(9)),
            "vmm_map_pre_lock": int(match.group(10)),
            "vmm_map_post_prep": int(match.group(11)),
            "vmm_map_put_op_wait": int(match.group(12)),
            "vmm_map_put_op_hold": int(match.group(13)),
            "map_dispatch": int(match.group(14)),
            "map_ack_poll": int(match.group(15)),
            "map_service": int(match.group(16)),
            "alloc_tail": int(match.group(17)),
            "unmap_wait": int(match.group(18)),
            "unmap_hold": int(match.group(19)),
            "unmap_pt": int(match.group(20)),
            "unmap_pre_lock": int(match.group(21)),
            "unmap_post_prep": int(match.group(22)),
            "unmap_put_op_wait": int(match.group(23)),
            "unmap_put_op_hold": int(match.group(24)),
            "unmap_dispatch": int(match.group(25)),
            "unmap_ack_poll": int(match.group(26)),
            "unmap_service": int(match.group(27)),
            "free_mid": int(match.group(28)),
            "pmm_free": int(match.group(29)),
            "free_tail": int(match.group(30)),
            "slot_free_wait": int(match.group(31)),
            "slot_free_hold": int(match.group(32)),
        })

    for match in re.finditer(
        r"\[KPROF_IPI_OVERLAY\] cpu=(\d+) remote_ipi_count=(\d+) remote_ipi_tsc=(\d+)",
        text,
    ):
        data["ipi_overlay"].append({
            "cpu": int(match.group(1)),
            "remote_ipi_count": int(match.group(2)),
            "remote_ipi_tsc": int(match.group(3)),
        })

    summary_match = re.search(
        r"\[KPROF_SUMMARY\] cpus=(\d+) total_ops=(\d+) overall_elapsed_tsc=(\d+) pmm_acq=(\d+) pmm_cont=(\d+) tlb_calls=(\d+) tlb_wait_cycles=(\d+)",
        text,
    )
    if summary_match:
        data["summary"] = {
            "cpus": int(summary_match.group(1)),
            "total_ops": int(summary_match.group(2)),
            "overall_elapsed_tsc": int(summary_match.group(3)),
            "pmm_acq": int(summary_match.group(4)),
            "pmm_cont": int(summary_match.group(5)),
            "tlb_calls": int(summary_match.group(6)),
            "tlb_wait_cycles": int(summary_match.group(7)),
        }

    sum_match = re.search(
        r"\[KPROF_SUBINTERVAL_SUM\] slot_alloc_wait=(\d+) slot_alloc_hold=(\d+) alloc_prep=(\d+) pmm_alloc=(\d+) map_prep=(\d+) "
        r"vmm_map_wait=(\d+) vmm_map_hold=(\d+) vmm_map_pt=(\d+) vmm_map_pre_lock=(\d+) vmm_map_post_prep=(\d+) "
        r"vmm_map_put_op_wait=(\d+) vmm_map_put_op_hold=(\d+) map_dispatch=(\d+) map_ack_poll=(\d+) map_service=(\d+) "
        r"alloc_tail=(\d+) unmap_wait=(\d+) unmap_hold=(\d+) unmap_pt=(\d+) unmap_pre_lock=(\d+) unmap_post_prep=(\d+) "
        r"unmap_put_op_wait=(\d+) unmap_put_op_hold=(\d+) unmap_dispatch=(\d+) unmap_ack_poll=(\d+) unmap_service=(\d+) "
        r"free_mid=(\d+) pmm_free=(\d+) free_tail=(\d+) slot_free_wait=(\d+) slot_free_hold=(\d+) "
        r"remote_ipi_count=(\d+) remote_ipi_tsc=(\d+)",
        text,
    )
    if sum_match:
        data["subinterval_sum"] = {
            "slot_alloc_wait": int(sum_match.group(1)),
            "slot_alloc_hold": int(sum_match.group(2)),
            "alloc_prep": int(sum_match.group(3)),
            "pmm_alloc": int(sum_match.group(4)),
            "map_prep": int(sum_match.group(5)),
            "vmm_map_wait": int(sum_match.group(6)),
            "vmm_map_hold": int(sum_match.group(7)),
            "vmm_map_pt": int(sum_match.group(8)),
            "vmm_map_pre_lock": int(sum_match.group(9)),
            "vmm_map_post_prep": int(sum_match.group(10)),
            "vmm_map_put_op_wait": int(sum_match.group(11)),
            "vmm_map_put_op_hold": int(sum_match.group(12)),
            "map_dispatch": int(sum_match.group(13)),
            "map_ack_poll": int(sum_match.group(14)),
            "map_service": int(sum_match.group(15)),
            "alloc_tail": int(sum_match.group(16)),
            "unmap_wait": int(sum_match.group(17)),
            "unmap_hold": int(sum_match.group(18)),
            "unmap_pt": int(sum_match.group(19)),
            "unmap_pre_lock": int(sum_match.group(20)),
            "unmap_post_prep": int(sum_match.group(21)),
            "unmap_put_op_wait": int(sum_match.group(22)),
            "unmap_put_op_hold": int(sum_match.group(23)),
            "unmap_dispatch": int(sum_match.group(24)),
            "unmap_ack_poll": int(sum_match.group(25)),
            "unmap_service": int(sum_match.group(26)),
            "free_mid": int(sum_match.group(27)),
            "pmm_free": int(sum_match.group(28)),
            "free_tail": int(sum_match.group(29)),
            "slot_free_wait": int(sum_match.group(30)),
            "slot_free_hold": int(sum_match.group(31)),
            "remote_ipi_count": int(sum_match.group(32)),
            "remote_ipi_tsc": int(sum_match.group(33)),
        }

    ctrl_summary_match = re.search(
        r"\[KPROF_CONTROL_SUMMARY\] cpus=(\d+) total_ops=(\d+) overall_elapsed_tsc=(\d+) pmm_acq=(\d+) pmm_cont=(\d+) tlb_calls=(\d+) tlb_wait_cycles=(\d+)",
        text,
    )
    if ctrl_summary_match:
        data["control_summary"] = {
            "cpus": int(ctrl_summary_match.group(1)),
            "total_ops": int(ctrl_summary_match.group(2)),
            "overall_elapsed_tsc": int(ctrl_summary_match.group(3)),
            "pmm_acq": int(ctrl_summary_match.group(4)),
            "pmm_cont": int(ctrl_summary_match.group(5)),
            "tlb_calls": int(ctrl_summary_match.group(6)),
            "tlb_wait_cycles": int(ctrl_summary_match.group(7)),
        }
    return data


def run_single_case(iso, out, label, cpus, mode="profile", firmware="bios", ram="2G", timeout=180):
    uart = out / (label + ".log")
    stderr = out / (label + ".stderr")
    command = [
        "qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", ram, "-smp", str(cpus),
        "-display", "none", "-monitor", "none", "-no-reboot", "-serial", f"file:{uart}",
        "-boot", "d", "-cdrom", str(iso),
    ]
    drives = []
    if firmware == "uefi":
        assert CODE.is_file() and VARS.is_file()
        variables = out / (label + "-vars.fd")
        shutil.copyfile(VARS, variables)
        drives = [
            f"if=pflash,format=raw,unit=0,readonly=on,file={CODE}",
            f"if=pflash,format=raw,unit=1,file={variables}",
        ]
        for drive in drives:
            command += ["-drive", drive]
    if mode == "profile":
        command += ["-fw_cfg", "name=opt/fortress/memory_kstack_profile,string=1"]
    elif mode == "control":
        command += ["-fw_cfg", "name=opt/fortress/memory_kstack_control,string=1"]

    assert [command[i + 1] for i, a in enumerate(command) if a == "-drive"] == drives
    assert not any(a in command for a in ("-device", "-blockdev", "-hda", "-hdb"))

    case_record = {
        "label": label,
        "cpus": cpus,
        "firmware": firmware,
        "ram": ram,
        "mode": mode,
        "enabled": (mode == "profile"),
        "argv": command,
        "status": "FAIL",
    }
    (out / (label + ".argv.json")).write_text(json.dumps(command, indent=2) + "\n")

    start = time.monotonic()
    with stderr.open("w") as err:
        child = subprocess.Popen(command, cwd=REPO, stdout=subprocess.DEVNULL, stderr=err)
        try:
            while time.monotonic() - start < timeout:
                text = uart.read_text(errors="replace") if uart.exists() else ""
                assert "[FAIL]" not in text, f"assertion failure in guest: {uart}"
                assert "KERNEL PANIC" not in text, f"kernel panic in guest: {uart}"
                if re.search(r"(?:fortress> |fortress:[^\r\n]* \$ )", text):
                    break
                assert child.poll() is None, f"QEMU exited early: {stderr}"
                time.sleep(0.1)
            else:
                raise TimeoutError(f"Shell prompt not reached within {timeout}s: {uart}")
        finally:
            if child.poll() is None:
                child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(timeout=5)

    duration = time.monotonic() - start
    case_record["elapsed_wall_seconds"] = round(duration, 2)
    output = uart.read_text(errors="replace")

    if mode == "profile":
        assert "[KPROF] exact_heap=PASS exact_tables=PASS zero_deferred=PASS pmm_recovered=PASS exact_bitmap=PASS" in output, (
            f"Missing post-profile recovery confirmation in {uart}"
        )
        assert "[KPROF] PASS kernel stack telemetry profiling complete" in output, (
            f"Missing profile completion marker in {uart}"
        )
        parsed = parse_kprof_log(output)
        assert parsed["summary"].get("cpus") == cpus, f"CPUs mismatch in telemetry: {parsed}"
        assert len(parsed["cpu_telemetry"]) == cpus, f"Telemetry CPU count mismatch: {parsed}"
        case_record["parsed_telemetry"] = parsed
    elif mode == "control":
        assert "[KPROF_CONTROL] exact_heap=PASS exact_tables=PASS zero_deferred=PASS pmm_recovered=PASS exact_bitmap=PASS" in output, (
            f"Missing control recovery confirmation in {uart}"
        )
        assert "[KPROF_CONTROL] PASS kernel stack untracked gated control complete" in output, (
            f"Missing control completion marker in {uart}"
        )
        parsed = parse_kprof_log(output)
        assert parsed["control_summary"].get("cpus") == cpus, f"CPUs mismatch in control: {parsed}"
        case_record["parsed_telemetry"] = parsed

    case_record["status"] = "PASS"
    return case_record


def compute_stats(values):
    if not values:
        return {"min": 0, "median": 0, "max": 0}
    s = sorted(values)
    n = len(s)
    med = s[n // 2] if n % 2 == 1 else round((s[n // 2 - 1] + s[n // 2]) / 2, 2)
    return {
        "min": s[0],
        "median": med,
        "max": s[-1],
    }


def main():
    timestamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    out = REPO / "build" / f"smp-memory-profile-{timestamp}"
    out.mkdir(parents=True)

    tracked = [REPO / "bin/fortress.img", REPO / "bin/fortress.elf", REPO / "docs/plans/PERMISSIONS_PLAN.md"]
    before = {str(p): digest(p) for p in tracked}
    record = {
        "status": "FAIL",
        "timestamp_utc": timestamp,
        "before_hashes": before,
        "matrix": [],
        "distributions": {},
    }

    try:
        with (out / "fixture.log").open("w") as log:
            import contextlib
            with contextlib.redirect_stdout(log):
                iso = make_iso(out)

        REPETITIONS = 3

        # Profiled runs at 1, 4, 8 CPUs
        for cpus in (1, 4, 8):
            for rep in range(1, REPETITIONS + 1):
                label = f"bios-smp{cpus}-profile-r{rep}"
                print(f"--> Running profiled case at {cpus} CPU(s) rep {rep}/{REPETITIONS}...", flush=True)
                res = run_single_case(iso, out, label, cpus=cpus, mode="profile")
                res["repetition"] = rep
                record["matrix"].append(res)
                print(f"    [PASS] {label} wall={res['elapsed_wall_seconds']}s", flush=True)

        # Matching untracked gated controls at 1, 4, 8 CPUs
        for cpus in (1, 4, 8):
            for rep in range(1, REPETITIONS + 1):
                label = f"bios-smp{cpus}-control-r{rep}"
                print(f"--> Running untracked gated control at {cpus} CPU(s) rep {rep}/{REPETITIONS}...", flush=True)
                res = run_single_case(iso, out, label, cpus=cpus, mode="control")
                res["repetition"] = rep
                record["matrix"].append(res)
                print(f"    [PASS] {label} wall={res['elapsed_wall_seconds']}s", flush=True)

        # Compute distributions and overhead comparison
        for cpus in (1, 4, 8):
            prof_cases = [c for c in record["matrix"] if c["cpus"] == cpus and c["mode"] == "profile"]
            ctrl_cases = [c for c in record["matrix"] if c["cpus"] == cpus and c["mode"] == "control"]

            prof_wall = [c["elapsed_wall_seconds"] for c in prof_cases]
            ctrl_wall = [c["elapsed_wall_seconds"] for c in ctrl_cases]
            prof_tsc = [c["parsed_telemetry"]["summary"]["overall_elapsed_tsc"] for c in prof_cases]
            ctrl_tsc = [c["parsed_telemetry"]["control_summary"]["overall_elapsed_tsc"] for c in ctrl_cases]
            tlb_waits = [c["parsed_telemetry"]["summary"]["tlb_wait_cycles"] for c in prof_cases]
            pmm_conts = [c["parsed_telemetry"]["summary"]["pmm_cont"] for c in prof_cases]
            pmm_acqs = [c["parsed_telemetry"]["summary"]["pmm_acq"] for c in prof_cases]

            avg_alloc_ticks = []
            avg_free_ticks = []
            for c in prof_cases:
                telems = c["parsed_telemetry"]["cpu_telemetry"]
                total_ops = sum(t["ops"] for t in telems)
                tot_alloc = sum(t["alloc_tsc_ticks"] for t in telems)
                tot_free = sum(t["free_tsc_ticks"] for t in telems)
                if total_ops > 0:
                    avg_alloc_ticks.append(round(tot_alloc / total_ops))
                    avg_free_ticks.append(round(tot_free / total_ops))

            sub_sums = {}
            sub_keys = (
                "slot_alloc_wait", "slot_alloc_hold", "alloc_prep", "pmm_alloc", "map_prep",
                "vmm_map_pre_lock", "vmm_map_wait", "vmm_map_hold", "vmm_map_pt", "vmm_map_post_prep", "vmm_map_put_op_wait", "vmm_map_put_op_hold",
                "map_dispatch", "map_ack_poll", "map_service",
                "alloc_tail", "unmap_pre_lock", "unmap_wait", "unmap_hold", "unmap_pt", "unmap_post_prep", "unmap_put_op_wait", "unmap_put_op_hold",
                "unmap_dispatch", "unmap_ack_poll", "unmap_service",
                "free_mid", "pmm_free", "free_tail", "slot_free_wait", "slot_free_hold", "remote_ipi_count", "remote_ipi_tsc"
            )
            for sub_key in sub_keys:
                vals = [c["parsed_telemetry"]["subinterval_sum"].get(sub_key, 0) for c in prof_cases]
                sub_sums[sub_key] = compute_stats(vals)

            ctrl_stats = compute_stats(ctrl_wall)
            prof_stats = compute_stats(prof_wall)
            ctrl_tsc_stats = compute_stats(ctrl_tsc)
            prof_tsc_stats = compute_stats(prof_tsc)
            gated_overhead_pct = round(((prof_tsc_stats["median"] - ctrl_tsc_stats["median"]) / ctrl_tsc_stats["median"]) * 100, 2) if ctrl_tsc_stats["median"] > 0 else 0
            wall_overhead_pct = round(((prof_stats["median"] - ctrl_stats["median"]) / ctrl_stats["median"]) * 100, 2) if ctrl_stats["median"] > 0 else 0

            # Reconciliation per run:
            # Layer 1: Complete Non-overlapping Partition
            # Alloc partition: slot_alloc_wait + slot_alloc_hold + alloc_prep + pmm_alloc + map_prep +
            #                  vmm_map_pre_lock + vmm_map_wait + vmm_map_hold + vmm_map_post_prep + vmm_map_put_op_wait + vmm_map_put_op_hold +
            #                  map_dispatch + map_ack_poll + map_service + alloc_tail == total_alloc_tsc
            # Free partition:  unmap_pre_lock + unmap_wait + unmap_hold + unmap_post_prep + unmap_put_op_wait + unmap_put_op_hold +
            #                  unmap_dispatch + unmap_ack_poll + unmap_service + free_mid + pmm_free + free_tail + slot_free_wait + slot_free_hold == total_free_tsc
            # Layer 2: Nested Overlay
            # remote_ipi_service_tsc & remote_ipi_count reported separately.
            reconciled_runs = []
            for c in prof_cases:
                sub = c["parsed_telemetry"]["subinterval_sum"]
                telems = c["parsed_telemetry"]["cpu_telemetry"]
                total_alloc = sum(t["alloc_tsc_ticks"] for t in telems)
                total_free = sum(t["free_tsc_ticks"] for t in telems)
                full = total_alloc + total_free

                alloc_partition = (sub["slot_alloc_wait"] + sub["slot_alloc_hold"] + sub["alloc_prep"] + sub["pmm_alloc"] +
                                   sub["map_prep"] + sub["vmm_map_pre_lock"] + sub["vmm_map_wait"] + sub["vmm_map_hold"] +
                                   sub["vmm_map_post_prep"] + sub["vmm_map_put_op_wait"] + sub["vmm_map_put_op_hold"] +
                                   sub["map_dispatch"] + sub["map_ack_poll"] + sub["map_service"] + sub["alloc_tail"])

                free_partition = (sub["unmap_pre_lock"] + sub["unmap_wait"] + sub["unmap_hold"] +
                                  sub["unmap_post_prep"] + sub["unmap_put_op_wait"] + sub["unmap_put_op_hold"] +
                                  sub["unmap_dispatch"] + sub["unmap_ack_poll"] + sub["unmap_service"] + sub["free_mid"] + sub["pmm_free"] +
                                  sub["free_tail"] + sub["slot_free_wait"] + sub["slot_free_hold"])

                alloc_residual = total_alloc - alloc_partition if total_alloc >= alloc_partition else 0
                free_residual = total_free - free_partition if total_free >= free_partition else 0
                total_partition = alloc_partition + free_partition
                residual = full - total_partition if full >= total_partition else 0

                # Explicit containment checks:
                assert total_alloc >= alloc_partition, f"Alloc underflow: {total_alloc} < {alloc_partition} in {c['label']}"
                assert total_free >= free_partition, f"Free underflow: {total_free} < {free_partition} in {c['label']}"

                kstack_wait = sub["slot_alloc_wait"] + sub["slot_free_wait"]
                kstack_hold = sub["slot_alloc_hold"] + sub["slot_free_hold"]
                pmm = sub["pmm_alloc"] + sub["pmm_free"]
                vmm_pre_lock = sub["vmm_map_pre_lock"] + sub["unmap_pre_lock"]
                vmm_wait = sub["vmm_map_wait"] + sub["unmap_wait"]
                vmm_hold = sub["vmm_map_hold"] + sub["unmap_hold"]
                vmm_post_prep = sub["vmm_map_post_prep"] + sub["unmap_post_prep"]
                vmm_put_op_wait = sub["vmm_map_put_op_wait"] + sub["unmap_put_op_wait"]
                vmm_put_op_hold = sub["vmm_map_put_op_hold"] + sub["unmap_put_op_hold"]
                vmm_misc = vmm_pre_lock + vmm_post_prep + vmm_put_op_wait + vmm_put_op_hold
                shootdowns = (sub["map_dispatch"] + sub["map_ack_poll"] + sub["map_service"] +
                              sub["unmap_dispatch"] + sub["unmap_ack_poll"] + sub["unmap_service"])
                wrappers = sub["alloc_prep"] + sub["map_prep"] + sub["alloc_tail"] + sub["free_mid"] + sub["free_tail"]

                tot_alloc_inner = sum(t["alloc_inner_tsc"] for t in telems)
                tot_free_inner = sum(t["free_inner_tsc"] for t in telems)
                full_inner = tot_alloc_inner + tot_free_inner
                invocation_gap_tsc = full - full_inner if full >= full_inner else 0
                internal_gap_tsc = full_inner - total_partition if full_inner >= total_partition else 0

                reconciled = {
                    "label": c["label"],
                    "repetition": c["repetition"],
                    "full_lifecycle_tsc": full,
                    "outer_vs_inner_audit": {
                        "outer_lifecycle_tsc": full,
                        "inner_operations_tsc": full_inner,
                        "invocation_gap_tsc": invocation_gap_tsc,
                        "invocation_gap_pct": round(invocation_gap_tsc / full * 100, 2) if full else 0,
                        "internal_subinterval_gap_tsc": internal_gap_tsc,
                        "internal_subinterval_gap_pct": round(internal_gap_tsc / full * 100, 2) if full else 0,
                    },
                    "layer1_partition": {
                        "kstack_lock_wait_tsc": kstack_wait,
                        "kstack_lock_hold_tsc": kstack_hold,
                        "pmm_tsc": pmm,
                        "vmm_pre_lock_tsc": vmm_pre_lock,
                        "vmm_lock_wait_tsc": vmm_wait,
                        "vmm_lock_hold_tsc": vmm_hold,
                        "vmm_post_prep_tsc": vmm_post_prep,
                        "vmm_put_op_wait_tsc": vmm_put_op_wait,
                        "vmm_put_op_hold_tsc": vmm_put_op_hold,
                        "vmm_misc_tsc": vmm_misc,
                        "shootdown_tsc": shootdowns,
                        "wrappers_tsc": wrappers,
                        "unmeasured_residual_tsc": residual,
                        "kstack_lock_wait_pct": round(kstack_wait / full * 100, 2) if full else 0,
                        "kstack_lock_hold_pct": round(kstack_hold / full * 100, 2) if full else 0,
                        "pmm_pct": round(pmm / full * 100, 2) if full else 0,
                        "vmm_pre_lock_pct": round(vmm_pre_lock / full * 100, 2) if full else 0,
                        "vmm_lock_wait_pct": round(vmm_wait / full * 100, 2) if full else 0,
                        "vmm_lock_hold_pct": round(vmm_hold / full * 100, 2) if full else 0,
                        "vmm_post_prep_pct": round(vmm_post_prep / full * 100, 2) if full else 0,
                        "vmm_put_op_wait_pct": round(vmm_put_op_wait / full * 100, 2) if full else 0,
                        "vmm_put_op_hold_pct": round(vmm_put_op_hold / full * 100, 2) if full else 0,
                        "vmm_misc_pct": round(vmm_misc / full * 100, 2) if full else 0,
                        "shootdown_pct": round(shootdowns / full * 100, 2) if full else 0,
                        "wrappers_pct": round(wrappers / full * 100, 2) if full else 0,
                        "unmeasured_residual_pct": round(residual / full * 100, 2) if full else 0,
                    },
                    "layer2_overlay": {
                        "remote_ipi_service_tsc": sub["remote_ipi_tsc"],
                        "remote_ipi_count": sub["remote_ipi_count"],
                        "remote_ipi_pct_of_lifecycle": round(sub["remote_ipi_tsc"] / full * 100, 2) if full else 0,
                    }
                }
                reconciled_runs.append(reconciled)
                c["reconciliation"] = reconciled

            record["distributions"][f"smp{cpus}"] = {
                "cpus": cpus,
                "repetitions": REPETITIONS,
                "control_wall_seconds": ctrl_stats,
                "profiled_wall_seconds": prof_stats,
                "control_gated_tsc": ctrl_tsc_stats,
                "profiled_gated_tsc": prof_tsc_stats,
                "wall_overhead_pct": wall_overhead_pct,
                "gated_overhead_pct": gated_overhead_pct,
                "overall_elapsed_tsc": compute_stats(prof_tsc),
                "avg_alloc_ticks_per_op": compute_stats(avg_alloc_ticks),
                "avg_free_ticks_per_op": compute_stats(avg_free_ticks),
                "tlb_wait_cycles": compute_stats(tlb_waits),
                "pmm_contention_events": compute_stats(pmm_conts),
                "pmm_acquires": compute_stats(pmm_acqs),
                "subintervals": sub_sums,
                "reconciliation_runs": reconciled_runs,
                "reconciliation_distribution": {
                    "full_lifecycle_tsc": compute_stats([r["full_lifecycle_tsc"] for r in reconciled_runs]),
                    "unmeasured_residual_tsc": compute_stats([r["layer1_partition"]["unmeasured_residual_tsc"] for r in reconciled_runs]),
                    "unmeasured_residual_pct": compute_stats([r["layer1_partition"]["unmeasured_residual_pct"] for r in reconciled_runs]),
                    "shootdown_pct": compute_stats([r["layer1_partition"]["shootdown_pct"] for r in reconciled_runs]),
                    "pmm_pct": compute_stats([r["layer1_partition"]["pmm_pct"] for r in reconciled_runs]),
                    "vmm_pre_lock_pct": compute_stats([r["layer1_partition"]["vmm_pre_lock_pct"] for r in reconciled_runs]),
                    "vmm_lock_wait_pct": compute_stats([r["layer1_partition"]["vmm_lock_wait_pct"] for r in reconciled_runs]),
                    "vmm_lock_hold_pct": compute_stats([r["layer1_partition"]["vmm_lock_hold_pct"] for r in reconciled_runs]),
                    "vmm_post_prep_pct": compute_stats([r["layer1_partition"]["vmm_post_prep_pct"] for r in reconciled_runs]),
                    "vmm_put_op_wait_pct": compute_stats([r["layer1_partition"]["vmm_put_op_wait_pct"] for r in reconciled_runs]),
                    "vmm_put_op_hold_pct": compute_stats([r["layer1_partition"]["vmm_put_op_hold_pct"] for r in reconciled_runs]),
                    "vmm_misc_pct": compute_stats([r["layer1_partition"]["vmm_misc_pct"] for r in reconciled_runs]),
                    "wrappers_pct": compute_stats([r["layer1_partition"]["wrappers_pct"] for r in reconciled_runs]),
                    "kstack_lock_wait_pct": compute_stats([r["layer1_partition"]["kstack_lock_wait_pct"] for r in reconciled_runs]),
                    "kstack_lock_hold_pct": compute_stats([r["layer1_partition"]["kstack_lock_hold_pct"] for r in reconciled_runs]),
                    "remote_ipi_service_tsc": compute_stats([r["layer2_overlay"]["remote_ipi_service_tsc"] for r in reconciled_runs]),
                    "remote_ipi_count": compute_stats([r["layer2_overlay"]["remote_ipi_count"] for r in reconciled_runs]),
                    "remote_ipi_pct_of_lifecycle": compute_stats([r["layer2_overlay"]["remote_ipi_pct_of_lifecycle"] for r in reconciled_runs]),
                    "invocation_gap_pct": compute_stats([r["outer_vs_inner_audit"]["invocation_gap_pct"] for r in reconciled_runs]),
                    "internal_subinterval_gap_pct": compute_stats([r["outer_vs_inner_audit"]["internal_subinterval_gap_pct"] for r in reconciled_runs]),
                },
                "shootdown_components_pct": {
                    "dispatch_pct": compute_stats([
                        round((c["parsed_telemetry"]["subinterval_sum"]["map_dispatch"] + c["parsed_telemetry"]["subinterval_sum"]["unmap_dispatch"]) /
                              (c["parsed_telemetry"]["subinterval_sum"]["map_dispatch"] + c["parsed_telemetry"]["subinterval_sum"]["unmap_dispatch"] +
                               c["parsed_telemetry"]["subinterval_sum"]["map_ack_poll"] + c["parsed_telemetry"]["subinterval_sum"]["unmap_ack_poll"] +
                               c["parsed_telemetry"]["subinterval_sum"]["map_service"] + c["parsed_telemetry"]["subinterval_sum"]["unmap_service"]) * 100, 1)
                        if (c["parsed_telemetry"]["subinterval_sum"]["map_dispatch"] + c["parsed_telemetry"]["subinterval_sum"]["unmap_dispatch"] +
                            c["parsed_telemetry"]["subinterval_sum"]["map_ack_poll"] + c["parsed_telemetry"]["subinterval_sum"]["unmap_ack_poll"] +
                            c["parsed_telemetry"]["subinterval_sum"]["map_service"] + c["parsed_telemetry"]["subinterval_sum"]["unmap_service"]) > 0 else 0
                        for c in prof_cases
                    ]),
                    "ack_poll_pct": compute_stats([
                        round((c["parsed_telemetry"]["subinterval_sum"]["map_ack_poll"] + c["parsed_telemetry"]["subinterval_sum"]["unmap_ack_poll"]) /
                              (c["parsed_telemetry"]["subinterval_sum"]["map_dispatch"] + c["parsed_telemetry"]["subinterval_sum"]["unmap_dispatch"] +
                               c["parsed_telemetry"]["subinterval_sum"]["map_ack_poll"] + c["parsed_telemetry"]["subinterval_sum"]["unmap_ack_poll"] +
                               c["parsed_telemetry"]["subinterval_sum"]["map_service"] + c["parsed_telemetry"]["subinterval_sum"]["unmap_service"]) * 100, 1)
                        if (c["parsed_telemetry"]["subinterval_sum"]["map_dispatch"] + c["parsed_telemetry"]["subinterval_sum"]["unmap_dispatch"] +
                            c["parsed_telemetry"]["subinterval_sum"]["map_ack_poll"] + c["parsed_telemetry"]["subinterval_sum"]["unmap_ack_poll"] +
                            c["parsed_telemetry"]["subinterval_sum"]["map_service"] + c["parsed_telemetry"]["subinterval_sum"]["unmap_service"]) > 0 else 0
                        for c in prof_cases
                    ]),
                    "service_pct": compute_stats([
                        round((c["parsed_telemetry"]["subinterval_sum"]["map_service"] + c["parsed_telemetry"]["subinterval_sum"]["unmap_service"]) /
                              (c["parsed_telemetry"]["subinterval_sum"]["map_dispatch"] + c["parsed_telemetry"]["subinterval_sum"]["unmap_dispatch"] +
                               c["parsed_telemetry"]["subinterval_sum"]["map_ack_poll"] + c["parsed_telemetry"]["subinterval_sum"]["unmap_ack_poll"] +
                               c["parsed_telemetry"]["subinterval_sum"]["map_service"] + c["parsed_telemetry"]["subinterval_sum"]["unmap_service"]) * 100, 1)
                        if (c["parsed_telemetry"]["subinterval_sum"]["map_dispatch"] + c["parsed_telemetry"]["subinterval_sum"]["unmap_dispatch"] +
                            c["parsed_telemetry"]["subinterval_sum"]["map_ack_poll"] + c["parsed_telemetry"]["subinterval_sum"]["unmap_ack_poll"] +
                            c["parsed_telemetry"]["subinterval_sum"]["map_service"] + c["parsed_telemetry"]["subinterval_sum"]["unmap_service"]) > 0 else 0
                        for c in prof_cases
                    ]),
                },
            }

        record["status"] = "PASS"
        print(f"\n[OK] All {len(record['matrix'])} SMP memory profile and control cases PASSED.", flush=True)

    except Exception as exc:
        record["status"] = "FAIL"
        record["error"] = str(exc)
        print(f"\n[FAIL] Campaign halted with error: {exc}", flush=True)
        raise
    finally:
        after = {str(p): digest(p) for p in tracked}
        record["after_hashes"] = after
        assert before[str(REPO / "docs/plans/PERMISSIONS_PLAN.md")] == after[str(REPO / "docs/plans/PERMISSIONS_PLAN.md")], (
            "PERMISSIONS_PLAN.md was modified during run"
        )
        assert before[str(REPO / "bin/fortress.img")] == after[str(REPO / "bin/fortress.img")], (
            "Default image was modified during test run"
        )
        result_file = out / "result.json"
        result_file.write_text(json.dumps(record, indent=2) + "\n")
        print(f"Saved authoritative record to {result_file}", flush=True)


if __name__ == "__main__":
    main()
