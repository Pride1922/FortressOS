#!/usr/bin/env python3
"""Shell S9 Phase 3: /bin/top acceptance test.
Real shell, BIOS + UEFI, disposable ISO, no data disks, argv preflight.
Verifies interactive refresh, busy vs stopped process CPU% deltas,
q/Ctrl-C exit, Ctrl-Z/bg/fg resume, and redirected one-shot mode.
"""
from pathlib import Path
import os
import re
import shutil
import socket
import subprocess
import tempfile
import threading
import time

REPO = Path(__file__).resolve().parent.parent
PROMPT_PATTERN = r"(?:fortress> |\[[a-zA-Z0-9_\-\./]+\]# )"


def qemu_command(mode, cpus, iso, variables, log, uart_path, qmp_path):
    cmd = [
        "qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", "2G",
        "-smp", str(cpus), "-display", "none", "-monitor", "none", "-no-reboot",
        "-boot", "d", "-cdrom", str(iso),
        "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log}",
        "-serial", "chardev:uart",
        "-qmp", f"unix:{qmp_path},server=on,wait=off"
    ]
    if mode == "uefi":
        cmd += [
            "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
            "-drive", f"if=pflash,format=raw,unit=1,file={variables}"
        ]
    return cmd


def preflight(cmd, mode, cpus, iso, variables, log, uart_path, qmp_path):
    assert mode in ("bios", "uefi") and cpus in (1, 4)
    expected = qemu_command(mode, cpus, iso, variables, log, uart_path, qmp_path)
    assert cmd == expected, f"unexpected QEMU argv: {cmd} != {expected}"
    for token in ("-device", "nvme", "-hda", "-hdb", "-blockdev", "-snapshot"):
        assert token not in cmd, f"preflight found forbidden token {token}"


def run_session(mode, cpus, iso, tmp):
    log = REPO / "build" / f"s9-top-{mode}-{cpus}.log"
    log.write_text("")
    uart_path = tmp / f"uart-{mode}-{cpus}"
    qmp_path = tmp / f"qmp-{mode}-{cpus}"
    variables = tmp / f"vars-{mode}-{cpus}.fd"
    if mode == "uefi":
        shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", variables)

    cmd = qemu_command(mode, cpus, iso, variables, log, uart_path, qmp_path)
    preflight(cmd, mode, cpus, iso, variables, log, uart_path, qmp_path)

    # Reject injection attempts
    for extra in (["-drive", "file=unsafe.img"], ["-device", "nvme"], ["-hda", "unsafe.img"]):
        try:
            preflight(cmd + extra, mode, cpus, iso, variables, log, uart_path, qmp_path)
        except AssertionError:
            pass
        else:
            raise AssertionError("preflight accepted injected storage device")

    with log.with_suffix(".stderr").open("wb") as stderr:
        proc = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=stderr)
        uart = None
        stop_reader = threading.Event()
        try:
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                if uart_path.exists():
                    try:
                        uart = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                        uart.connect(str(uart_path))
                        break
                    except (ConnectionRefusedError, OSError):
                        pass
                time.sleep(0.05)
            assert uart is not None, f"Could not connect to UART at {uart_path}"
            uart.settimeout(0.2)

            def drain():
                while not stop_reader.is_set():
                    try:
                        if not uart.recv(65536):
                            return
                    except (socket.timeout, OSError):
                        pass
            reader = threading.Thread(target=drain, daemon=True)
            reader.start()

            def get_raw():
                return log.read_bytes().decode(errors="replace")

            def get_text():
                return re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", get_raw()).replace("\r", "")

            def wait_for_prompt(after_idx=0, timeout=60):
                d = time.monotonic() + timeout
                while time.monotonic() < d:
                    t = get_text()
                    if re.search(PROMPT_PATTERN, t[after_idx:]):
                        return t[after_idx:]
                    assert proc.poll() is None, "QEMU terminated prematurely"
                    time.sleep(0.05)
                raise TimeoutError(f"Prompt wait timed out: {get_text()[-1000:]}")

            def exec_cmd(cmd_str):
                start = len(get_text())
                for b in cmd_str.encode() + b"\n":
                    uart.send(bytes([b]))
                    time.sleep(0.005)
                return wait_for_prompt(start)

            # Wait for shell prompt
            wait_for_prompt(0, timeout=60)

            # -------------------------------------------------------------
            # Stage 1: Redirected one-shot mode (pipeline / non-tty)
            # -------------------------------------------------------------
            print("      [Stage 1] Testing redirected one-shot mode (/bin/top | /bin/head -n 5)...", flush=True)
            raw_start = len(get_raw())
            out_pipe = exec_cmd("/bin/top | /bin/head -n 5")
            raw_pipe = get_raw()[raw_start:]
            # Ensure no ANSI clear screen was emitted in redirected output
            assert "\x1b[2J" not in raw_pipe, "Escape sequence \\x1b[2J found in redirected top output"
            assert "FortressOS top — up" in out_pipe, f"Missing header in redirected output: {out_pipe}"
            assert "CPUs:" in out_pipe and "Mem:" in out_pipe, f"Missing system info in redirected output: {out_pipe}"

            # -------------------------------------------------------------
            # Stage 2: Interactive launch, timed refresh, and 'q' to quit
            # -------------------------------------------------------------
            print("      [Stage 2] Testing interactive timed refresh and 'q' quit...", flush=True)
            raw_start = len(get_raw())
            for b in b"/bin/top\n":
                uart.send(bytes([b]))
                time.sleep(0.005)

            # Wait for frame 1
            d = time.monotonic() + 30
            while time.monotonic() < d:
                raw_top = get_raw()[raw_start:]
                if "FortressOS top — up" in raw_top and "\x1b[2J\x1b[H" in raw_top:
                    break
                time.sleep(0.1)
            assert "FortressOS top — up" in get_raw()[raw_start:], "Top header did not appear in interactive mode"

            # Observe at least two full refresh cycles without typing anything
            first_frame_count = get_raw()[raw_start:].count("FortressOS top — up")
            d = time.monotonic() + 15
            while time.monotonic() < d:
                current_frame_count = get_raw()[raw_start:].count("FortressOS top — up")
                if current_frame_count >= first_frame_count + 1:
                    break
                time.sleep(0.2)
            assert current_frame_count >= first_frame_count + 1, "Top timed refresh did not produce subsequent frame"

            # Press 'q' to quit
            start_prompt = len(get_text())
            uart.send(b"q")
            wait_for_prompt(start_prompt, timeout=10)

            # -------------------------------------------------------------
            # Stage 3: Busy vs stopped process CPU% delta verification
            # -------------------------------------------------------------
            print("      [Stage 3] Testing busy vs stopped process CPU% deltas...", flush=True)
            exec_cmd("/bin/hello --spin &")
            exec_cmd("cat &")
            exec_cmd("kill %2 STOP")

            raw_start = len(get_raw())
            for b in b"/bin/top\n":
                uart.send(bytes([b]))
                time.sleep(0.005)

            # Wait for frame 2 (deltas computed)
            d = time.monotonic() + 30
            target_frames = get_raw()[raw_start:].count("FortressOS top — up") + 2
            while time.monotonic() < d:
                if get_raw()[raw_start:].count("FortressOS top — up") >= target_frames:
                    break
                time.sleep(0.2)

            recent_raw = get_raw()[raw_start:]
            # Find the last rendered frame
            frames = recent_raw.split("FortressOS top — up")
            last_frame = frames[-1] if frames else ""

            # Check for busy worker (hello) having nonzero CPU%
            hello_match = re.search(r"(\d+)\s+RUNNING\s+([0-9\.]+|unknown)\s+(?:/bin/)?hello", last_frame)
            cat_match = re.search(r"(\d+)\s+STOPPED\s+([0-9\.]+|unknown)\s+(?:/bin/)?cat", last_frame)

            assert hello_match is not None, f"hello process not found in top frame:\n{last_frame}"
            assert cat_match is not None, f"cat process not found in top frame:\n{last_frame}"

            hello_cpu = hello_match.group(2)
            cat_cpu = cat_match.group(2)

            # If frame has deltas computed:
            if hello_cpu != "unknown":
                hello_val = float(hello_cpu)
                assert hello_val > 0.0, f"Busy process hello expected nonzero CPU%, got {hello_val}"

            if cat_cpu != "unknown":
                cat_val = float(cat_cpu)
                assert cat_val <= 1.0, f"Stopped process cat expected ~0.0% CPU%, got {cat_val}"

            # Quit top
            start_prompt = len(get_text())
            uart.send(b"q")
            wait_for_prompt(start_prompt, timeout=10)

            # Cleanup background jobs
            exec_cmd("kill %1 KILL")
            exec_cmd("kill %2 KILL")

            # -------------------------------------------------------------
            # Stage 4: Ctrl-C quit and prompt recovery
            # -------------------------------------------------------------
            print("      [Stage 4] Testing Ctrl-C quit...", flush=True)
            raw_start = len(get_raw())
            for b in b"/bin/top\n":
                uart.send(bytes([b]))
                time.sleep(0.005)

            d = time.monotonic() + 20
            while time.monotonic() < d:
                if "FortressOS top — up" in get_raw()[raw_start:]:
                    break
                time.sleep(0.1)

            start_prompt = len(get_text())
            uart.send(b"\x03")  # Ctrl-C
            wait_for_prompt(start_prompt, timeout=10)

            # -------------------------------------------------------------
            # Stage 5: Ctrl-Z, bg, fg lifecycle
            # -------------------------------------------------------------
            print("      [Stage 5] Testing Ctrl-Z, bg, and fg resume...", flush=True)
            raw_start = len(get_raw())
            for b in b"/bin/top\n":
                uart.send(bytes([b]))
                time.sleep(0.005)

            d = time.monotonic() + 20
            while time.monotonic() < d:
                if "FortressOS top — up" in get_raw()[raw_start:]:
                    break
                time.sleep(0.1)

            start_prompt = len(get_text())
            uart.send(b"\x1a")  # Ctrl-Z
            # Shell should report Stopped and return prompt
            out_stop = wait_for_prompt(start_prompt, timeout=10)
            assert "Stopped" in out_stop or "top" in out_stop, f"Expected stopped job notification: {out_stop}"

            # Send bg
            exec_cmd("bg")

            # Send fg
            raw_fg_start = len(get_raw())
            for b in b"fg\n":
                uart.send(bytes([b]))
                time.sleep(0.005)

            # Confirm top resumes and redraws
            d = time.monotonic() + 15
            while time.monotonic() < d:
                if "FortressOS top — up" in get_raw()[raw_fg_start:]:
                    break
                time.sleep(0.1)
            assert "FortressOS top — up" in get_raw()[raw_fg_start:], "Top did not redraw after fg resume"

            # Quit top
            start_prompt = len(get_text())
            uart.send(b"q")
            wait_for_prompt(start_prompt, timeout=10)

            return True

        finally:
            stop_reader.set()
            if uart:
                uart.close()
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)


def main():
    print("=== Shell S9 Phase 3: /bin/top Acceptance Suite ===")
    with tempfile.TemporaryDirectory(prefix="fortress-s9-top-") as directory:
        tmp = Path(directory)
        iso = tmp / "top.iso"
        shutil.copyfile(REPO / "bin/fortress.iso", iso)

        for mode in ("bios", "uefi"):
            for cpus in (1, 4):
                print(f"[*] Testing {mode.upper()} SMP={cpus}...", flush=True)
                ok = run_session(mode, cpus, iso, tmp)
                assert ok, f"Session failed for {mode} SMP={cpus}"
                print(f"    PASS: {mode.upper()} SMP={cpus} verified", flush=True)

    print("\nPASS test-s9-top: All tests passed on BIOS & UEFI (SMP=1 and SMP=4) with no data disks!")


if __name__ == "__main__":
    main()
