#!/usr/bin/env python3
"""BIOS/UEFI TCG panic reproduction; disposable ISO only, no data disks.

Debugger is disconnected throughout execution. On panic/timeout, QMP stops
all CPUs before register/stack capture and optional physical RAM ELF dump.
These diagnostic runs are not performance trials.
"""
import argparse
import hashlib
import json
import re
import shutil
import socket
import subprocess
import tempfile
import threading
import time
from pathlib import Path

from test_nmi_transitions import QMP, connect
from test_smpbench_qemu import PROMPT_PATTERN

REPO = Path(__file__).resolve().parent.parent


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def gdb_capture(path, elf, out):
    # Catch each failed read independently: one corrupt/unmapped stack must not
    # suppress other CPU registers or the remaining diagnostic objects.
    commands = ["set pagination off", "set confirm off", "set debuginfod enabled off",
                "set auto-load off", "set print elements 64", "set print max-depth 6",
                "set max-value-size 1048576",
                "set backtrace limit 32", "set remotetimeout 10",
                "target remote " + str(path), "python", "import gdb, pathlib",
                "out = pathlib.Path(" + repr(str(out)) + ")",
                "def attempt(command):",
                "    print('\\n>>> ' + command)",
                "    try: gdb.execute(command)",
                "    except Exception as error: print('CAPTURE_READ_ERROR:', error)"]
    for command in ["info threads", "thread apply all info all-registers",
                    "thread apply all bt full", "thread apply all x/128gx $rsp",
                    "thread apply all x/32bx $rip", "thread apply all x/16i $rip",
                    "p cpu_locals", "p scheduler_cpus", "p g_smp_tlb_mailboxes",
                    "p/x g_smp_tlb_active_initiators", "p g_vmm_spaces_list",
                    "p g_vmm_deferred_list"]:
        commands.append("attempt(" + repr(command) + ")")
    commands += ["for thread in gdb.selected_inferior().threads():",
                 "    thread.switch()",
                 "    attempt('p scheduler_cpus[%d]' % (thread.num - 1))",
                 "    attempt('p *cpu_locals[%d].current_thread' % (thread.num - 1))",
                 "for slot in range(64):",
                 "    base = 0xffffffffa0000000 + slot * 20480 + 4096",
                 "    try:",
                 "        data = bytes(gdb.selected_inferior().read_memory(base, 16384))",
                 "        (out / ('stack-slot-%02d.bin' % slot)).write_bytes(data)",
                 "    except Exception as error: print('STACK_SLOT_UNAVAILABLE', slot, error)",
                 "end", "disconnect"]
    script = out / "capture.gdb"
    script.write_text("\n".join(commands) + "\n")
    with (out / "gdb.txt").open("w") as log:
        result = subprocess.run(["gdb", "-nx", "-nh", "-batch", "-q", str(elf),
                                 "-x", str(script)], stdout=log, stderr=subprocess.STDOUT,
                                timeout=45, cwd=REPO)
    return result.returncode


def capture(qmp, debugger, elf, out, cpus, dump_ram):
    qmp.execute("stop")
    state = qmp.execute("query-status")
    assert not state["running"], "Guest must be stopped before capture"
    registers = {}
    for cpu in range(cpus):
        registers[str(cpu)] = qmp.execute("human-monitor-command", {
            "command-line": "info registers", "cpu-index": cpu})
    (out / "qemu-registers.json").write_text(json.dumps(registers, indent=2))
    result = {"status": state, "gdb_exit_code": gdb_capture(debugger, elf, out)}
    if dump_ram:
        qmp.execute("dump-guest-memory", {"paging": False,
                     "protocol": "file:" + str(out / "guest-memory.elf")})
        result["ram_dump"] = "guest-memory.elf"
    result["final_status"] = qmp.execute("query-status")
    return result


def run(args, iso, elf, directory, number):
    out = directory / f"run-{number:02d}"
    out.mkdir()
    record = {"run": number, "outcome": "incomplete", "commands": []}
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="fortress-panic-") as temporary:
        sockets = Path(temporary)
        uart_path, qmp_path, debugger = [sockets / name for name in ("uart", "qmp", "gdb")]
        cmd = ["qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", "2G",
               "-smp", str(args.cpus), "-display", "none", "-no-reboot", "-monitor", "none",
               "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={out / 'serial.log'}",
               "-serial", "chardev:uart", "-boot", "d", "-cdrom", str(iso),
               "-qmp", f"unix:{qmp_path},server=on,wait=off",
               "-gdb", f"unix:{debugger},server=on,wait=off"]
        if args.firmware == "uefi":
            code = Path("/usr/share/OVMF/OVMF_CODE_4M.fd")
            variables = out / "OVMF_VARS_4M.fd"
            shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", variables)
            cmd += ["-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={code}",
                    "-drive", f"if=pflash,format=raw,unit=1,file={variables}"]
        assert not any(value in cmd for value in ("-blockdev", "-device", "-hda", "-hdb", "-sd"))
        drives = [cmd[i + 1] for i, value in enumerate(cmd) if value == "-drive"]
        assert drives == ([] if args.firmware == "bios" else [
            f"if=pflash,format=raw,unit=0,readonly=on,file={code}",
            f"if=pflash,format=raw,unit=1,file={variables}"]), "Unexpected data disk"
        record["qemu_argv"] = cmd
        (out / "manifest.json").write_text(json.dumps(record, indent=2))
        stop = threading.Event()
        uart = qmp = None
        reader = None
        with (out / "qemu.stderr").open("w") as errors:
            proc = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=errors)
            try:
                uart = connect(uart_path)
                uart.settimeout(0.2)

                def drain():
                    while not stop.is_set():
                        try:
                            if not uart.recv(65536): return
                        except socket.timeout:
                            continue
                        except OSError:
                            return
                reader = threading.Thread(target=drain, daemon=True)
                reader.start()
                qmp = QMP(qmp_path)

                def output():
                    log = out / "serial.log"
                    data = log.read_bytes().decode(errors="replace") if log.exists() else ""
                    return re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", data).replace("\r", "")

                def await_prompt(start, timeout):
                    deadline = time.monotonic() + timeout
                    while time.monotonic() < deadline:
                        text = output()[start:]
                        if "KERNEL PANIC" in text or "[FATAL]" in text:
                            raise RuntimeError("guest-panic")
                        if PROMPT_PATTERN.search(text): return text
                        if proc.poll() is not None: raise RuntimeError("qemu-exited")
                        time.sleep(0.025)
                    raise TimeoutError("guest-timeout")

                await_prompt(0, 45)
                for workload in args.workloads:
                    profile_flag = " -p" if args.profile else " --wait-profile" if args.wait_profile else ""
                    for command in ("lockstat -c", f"smpbench -w {workload} -r {args.reps} -c{profile_flag}", "lockstat -c"):
                        start = len(output())
                        record["commands"].append(command)
                        for byte in (command + "\n").encode():
                            uart.sendall(bytes([byte]))
                            time.sleep(0.005)
                        reply = await_prompt(start, args.timeout)
                        if command.startswith("smpbench"):
                            assert re.search(r"^w=" + workload + r" .*ok=1 short=0", reply, re.M), "benchmark-failed"
                            if args.profile:
                                from test_smpbench_qemu import validate_profile
                                validate_profile(reply, args.cpus, args.reps)
                            elif args.require_barrier or args.wait_profile:
                                from test_smpbench_qemu import validate_barrier
                                validate_barrier(reply, args.cpus, args.reps)
                            if args.wait_profile:
                                assert reply.count('wait_diag=1') == args.cpus * (args.reps + 1), 'Missing wait-only diagnostics'
                record["outcome"] = "no-panic-observed"
                if args.snapshot_after_run:
                    record["capture"] = capture(qmp, debugger, elf, out, args.cpus, False)
            except Exception as error:
                record["outcome"] = str(error)
                if qmp is not None and proc.poll() is None:
                    try:
                        record["capture"] = capture(qmp, debugger, elf, out, args.cpus, args.dump_ram)
                    except Exception as capture_error:
                        record["capture_error"] = str(capture_error)
            finally:
                record["elapsed_seconds"] = round(time.monotonic() - started, 3)
                record["qemu_exit_before_cleanup"] = proc.poll()
                proc.terminate()
                try: proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)
                record["qemu_exit_after_cleanup"] = proc.returncode
                stop.set()
                if reader is not None: reader.join(timeout=1)
                if uart is not None: uart.close()
                if qmp is not None:
                    qmp.stream.close()
                    qmp.sock.close()
                (out / "manifest.json").write_text(json.dumps(record, indent=2))
    print(f"run={number} outcome={record['outcome']} evidence={out}", flush=True)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, required=True)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cpus", type=int, choices=(1, 4, 8), default=8)
    parser.add_argument("--firmware", choices=("bios", "uefi"), default="bios")
    parser.add_argument("--require-barrier", action="store_true", help="Validate rev=2 readiness and start-spread evidence.")
    parser.add_argument("--profile", action="store_true", help="Run opt-in phase profiling and validate its accounting.")
    parser.add_argument("--wait-profile", action="store_true", help="Scheduler wait clocks without per-call phase clocks.")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--reps", type=int, default=7)
    parser.add_argument("--timeout", type=int, default=90)
    parser.add_argument("--workloads", nargs="+", choices=("cpu_scale", "spawn_wait", "signals", "pipes"),
                        default=["cpu_scale", "spawn_wait", "signals", "pipes"])
    parser.add_argument("--dump-ram", action="store_true")
    parser.add_argument("--snapshot-after-run", action="store_true", help="Validate capture on a healthy, stopped guest.")
    args = parser.parse_args()
    assert 1 <= args.runs <= 20 and 1 <= args.reps <= 31 and 10 <= args.timeout <= 300
    assert shutil.which("gdb"), "Install GDB before starting reproduction"
    directory = args.output.resolve()
    directory.mkdir(parents=True, exist_ok=False)
    iso, elf = directory / "fortress.iso", directory / "fortress.elf"
    shutil.copyfile(args.iso.resolve(), iso)
    shutil.copyfile(args.elf.resolve(), elf)
    # Reject mismatched debug symbols, even when filenames appear to match.
    with tempfile.TemporaryDirectory(prefix="fortress-symbol-check-") as temporary:
        embedded = Path(temporary) / "kernel.elf"
        subprocess.run(["xorriso", "-osirrox", "on", "-indev", str(iso),
                        "-extract", "/boot/fortress.elf", str(embedded)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
        assert digest(embedded) == digest(elf), "ELF does not match ISO kernel"
    manifest = {"iso_sha256": digest(iso), "elf_sha256": digest(elf),
                "qemu_version": subprocess.check_output(["qemu-system-x86_64", "--version"], text=True),
                "debugger_policy": "Attach only after QMP stop; no breakpoints, stepping, or guest memory writes",
                "parameters": {"cpus": args.cpus, "reps": args.reps, "firmware": args.firmware,
                               "require_barrier": args.require_barrier,
                                "profile": args.profile, "wait_profile": args.wait_profile,
                               "workloads": args.workloads, "timeout": args.timeout,
                               "dump_ram": args.dump_ram},
                "runs": []}
    try:
        for number in range(1, args.runs + 1):
            result = run(args, iso, elf, directory, number)
            manifest["runs"].append(result)
            (directory / "manifest.json").write_text(json.dumps(manifest, indent=2))
            if result["outcome"] != "no-panic-observed": break
    finally:
        (directory / "manifest.json").write_text(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
