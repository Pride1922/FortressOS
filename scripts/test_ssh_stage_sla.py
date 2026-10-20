#!/usr/bin/env python3
"""Interactive SSH Keystroke & Command SLA Gate during Active 32-Block Staging.

Evaluates whether synchronous 32-block (128 KiB) ext4 staging batches harm interactive
SSH responsiveness. Measures:
  1. Idle SSH keystroke round-trip echo latency (50 samples).
  2. Idle command execution roundtrip.
  3. Active staging SSH keystroke round-trip echo latency (150 samples) while
     /bin/stage-probe is actively executing 32-block batch appends (10M / 100M).
  4. Active staging command execution roundtrip.

Acceptance Gates per EXT4_STAGING_BATCH_DESIGN.md:
  - SSH Keystroke p95 < 100 ms
  - SSH Command Roundtrip < 500 ms
"""

from pathlib import Path
import json
import os
import re
import select
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))
from test_nmi_transitions import connect
from test_ext4_read import gpt
from test_ext4_time_host import fixture
from test_jbd2_replay_host import sbseal
from create_ext4_fixtures import ROOT, run

PROMPT_PATTERN = r"(?:fortress> |(?:\[-?\d+\] )?fortress:[^\r\n]* \$ |\[[a-zA-Z0-9_\-\./]+\]# |[A-Za-z0-9_-]+@[A-Za-z0-9-]+:[^\r\n]*[#$] )"
CODE = Path("/usr/share/OVMF/OVMF_CODE_4M.fd")
VARS = Path("/usr/share/OVMF/OVMF_VARS_4M.fd")
ISO = REPO / "bin" / "fortress.iso"
TEST_PASSWORD = "ssh-acceptance-pass"


def find_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def create_ext4_nvme_fixture(dest_img: Path, size_mib: int = 160):
    """Creates a 160 MiB GPT NVMe ext4 partition with valid journal and .fortress config."""
    with tempfile.TemporaryDirectory(prefix="fortress-sla-ext4-") as tmp:
        tmp_path = Path(tmp)
        base = fixture(tmp_path, 4096, size_mib=size_mib)
        run(['debugfs', '-w', '-R', 'rm /target.bin', str(base)])

        dot_fortress = tmp_path / ".fortress"
        dot_fortress.mkdir()
        sshd_conf = dot_fortress / "sshd.conf"
        sshd_conf.write_text(
            "Enabled yes\n"
            "Port 22\n"
            "ListenAddress 0.0.0.0\n"
            "PermitRootLogin no\n"
            "PasswordAuthentication yes\n"
            "PubkeyAuthentication yes\n"
            "MaxSessions 2\n"
            "MaxAuthTries 5\n"
            "LoginGraceTime 60\n"
        )
        host_key = dot_fortress / "ssh_host_ed25519_key"
        subprocess.run(
            ["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "fortress-test-host", "-f", str(host_key)],
            check=True
        )

        run(['debugfs', '-w', '-R', 'mkdir /.fortress', str(base)])
        run(['debugfs', '-w', '-R', f'write {sshd_conf} /.fortress/sshd.conf', str(base)])
        run(['debugfs', '-w', '-R', f'write {host_key} /.fortress/ssh_host_ed25519_key', str(base)])

        data = bytearray(base.read_bytes())
        sbseal(data)
        base.write_bytes(data)
        run(['e2fsck', '-fn', str(base)])

        gpt(base, dest_img)


def build_test_iso(out_iso: Path, password: str, probe_elf: Path):
    """Builds a test ISO containing operator password in etc/shadow and /bin/stage-probe."""
    tmp = out_iso.parent
    iso_in = REPO / 'build/iso_root'
    iso_out = tmp / 'iso_root'
    if iso_out.exists():
        shutil.rmtree(iso_out)
    shutil.copytree(iso_in, iso_out)
    shutil.copyfile(REPO / 'bin/fortress.elf', iso_out / 'boot/fortress.elf')

    res = subprocess.run(['openssl', 'passwd', '-5', '-salt', '12345678', password],
                         capture_output=True, text=True, check=True)
    h = res.stdout.strip()

    tar_in = REPO / 'bin/initramfs.tar'
    tar_out = tmp / 'initramfs.tar'
    import tarfile, io
    with tarfile.open(tar_in) as src, tarfile.open(tar_out, 'w', format=tarfile.USTAR_FORMAT) as dst:
        for m in src.getmembers():
            if m.name == 'etc/shadow':
                content = f'root:!:::::::\noperator:{h}:::::::\n'.encode()
                m.size = len(content)
                dst.addfile(m, io.BytesIO(content))
            else:
                data = src.extractfile(m).read() if m.isfile() else None
                dst.addfile(m, io.BytesIO(data) if data is not None else None)

        probe_data = probe_elf.read_bytes()
        ti = tarfile.TarInfo(name='bin/stage-probe')
        ti.size = len(probe_data)
        ti.mode = 0o755
        ti.type = tarfile.REGTYPE
        dst.addfile(ti, io.BytesIO(probe_data))

    shutil.copyfile(tar_out, iso_out / 'boot/initramfs.tar')
    shutil.copyfile(tar_out, iso_out / 'initramfs.tar')

    subprocess.run([
        'xorriso', '-as', 'mkisofs', '-b', 'boot/limine/limine-bios-cd.bin',
        '-no-emul-boot', '-boot-load-size', '4', '-boot-info-table',
        '--efi-boot', 'boot/limine/limine-uefi-cd.bin',
        '-efi-boot-part', '--efi-boot-image', '--protective-msdos-label',
        str(iso_out), '-o', str(out_iso)
    ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(['limine/limine', 'bios-install', str(out_iso)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def measure_keystroke_stream(client, stream_text: str, delay: float = 0.025):
    """Sends individual characters and measures round-trip echo latency."""
    latencies = []
    received = b""
    for ch in stream_text:
        t0 = time.monotonic()
        client.stdin.write(ch.encode())
        client.stdin.flush()

        ch_bytes = ch.encode()
        got_echo = False
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            ready, _, _ = select.select([client.stdout], [], [], 0.005)
            if ready:
                chunk = os.read(client.stdout.fileno(), 1024)
                if chunk:
                    received += chunk
                    if ch_bytes in chunk:
                        t1 = time.monotonic()
                        latencies.append((t1 - t0) * 1000.0)
                        got_echo = True
                        break
        assert got_echo, f"Timeout waiting for echo of character {ch!r} (received: {received!r})"
        if delay > 0:
            time.sleep(delay)
    return latencies


def measure_command_roundtrip(client, command: str, expected_token: str, timeout: float = 10.0):
    """Sends a command line and measures roundtrip execution time until output token is received."""
    t0 = time.monotonic()
    client.stdin.write((command + "\r").encode())
    client.stdin.flush()

    token_bytes = expected_token.encode()
    deadline = time.monotonic() + timeout
    output = b""
    while time.monotonic() < deadline:
        ready, _, _ = select.select([client.stdout], [], [], 0.01)
        if ready:
            chunk = os.read(client.stdout.fileno(), 4096)
            if chunk:
                output += chunk
                if token_bytes in output:
                    t1 = time.monotonic()
                    return (t1 - t0) * 1000.0, output.decode(errors="replace")
    raise AssertionError(f"Timeout waiting for command token {expected_token!r}; output tail: {output[-500:]!r}")


def compute_stats(latencies):
    s = sorted(latencies)
    n = len(s)
    return {
        "count": n,
        "min_ms": round(s[0], 2),
        "mean_ms": round(sum(s) / n, 2),
        "median_ms": round(s[n // 2], 2),
        "p95_ms": round(s[int(n * 0.95)], 2),
        "p99_ms": round(s[min(int(n * 0.99), n - 1)], 2),
        "max_ms": round(s[-1], 2),
    }


def run_sla_test(firmware="bios", smp=4):
    print(f"\n==================================================================", flush=True)
    print(f" FortressOS Interactive SSH SLA Gate ({firmware.upper()}, SMP={smp})", flush=True)
    print(f"==================================================================", flush=True)

    with tempfile.TemporaryDirectory(prefix=f"fortress-sla-{firmware}-smp{smp}-") as tmp:
        tmp_dir = Path(tmp)
        uart_path = tmp_dir / "uart"
        log_path = tmp_dir / "serial.log"
        nvme_path = tmp_dir / "nvme.img"
        test_iso = tmp_dir / "test.iso"

        probe_elf = REPO / "build/stage-probe.elf"
        assert probe_elf.is_file(), "stage-probe.elf not found; build it first"

        client_key = tmp_dir / "client_id_ed25519"
        subprocess.run(
            ["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "fortress-sla-client", "-f", str(client_key)],
            check=True
        )
        client_pub_line = client_key.with_suffix(".pub").read_text()

        print(f"  [1] Creating 160 MiB EXT4 journaled NVMe fixture...", flush=True)
        create_ext4_nvme_fixture(nvme_path, size_mib=160)

        print(f"  [2] Creating staged operator ISO with /bin/stage-probe...", flush=True)
        build_test_iso(test_iso, TEST_PASSWORD, probe_elf)

        host_port = find_free_port()
        print(f"  [3] Allocated port: 127.0.0.1:{host_port} -> guest:22", flush=True)

        qemu_cmd = [
            "qemu-system-x86_64", "-M", "q35", "-cpu", "max,+rdrand,+rdseed", "-m", "2G", "-smp", str(smp),
            "-display", "none", "-no-reboot", "-boot", "d", "-cdrom", str(test_iso),
            "-drive", f"file={nvme_path},if=none,id=nvm0,format=raw,snapshot=off",
            "-device", "nvme,serial=LATENCYFIXTURE,drive=nvm0",
            "-fw_cfg", "name=opt/fortress/ext4_journal_test,string=1",
            "-netdev", f"user,id=net0,hostfwd=tcp:127.0.0.1:{host_port}-:22",
            "-device", "e1000,netdev=net0",
            "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log_path}",
            "-serial", "chardev:uart"
        ]

        if firmware == "uefi":
            vars_path = tmp_dir / "vars.fd"
            shutil.copyfile(VARS, vars_path)
            qemu_cmd += [
                "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={CODE}",
                "-drive", f"if=pflash,format=raw,unit=1,file={vars_path}"
            ]

        proc = subprocess.Popen(qemu_cmd, cwd=REPO)
        try:
            uart = connect(uart_path)
            stop_drain = threading.Event()
            log_buffer = bytearray()

            def drain():
                uart.settimeout(0.2)
                while not stop_drain.is_set():
                    try:
                        chunk = uart.recv(4096)
                        if chunk:
                            log_buffer.extend(chunk)
                    except socket.timeout:
                        continue
                    except Exception:
                        break

            drain_thread = threading.Thread(target=drain, daemon=True)
            drain_thread.start()

            def get_text():
                raw = log_buffer.decode("utf-8", errors="replace")
                return re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", raw)

            def send_str(s):
                for b in s.encode():
                    uart.send(bytes([b]))
                    time.sleep(0.005)

            def wait_for_prompt(start_pos=0, timeout=60, require_newline=False):
                deadline = time.monotonic() + timeout
                sent_user = False
                sent_pass = False
                while time.monotonic() < deadline:
                    txt = get_text()
                    chunk = txt[start_pos:]
                    if not sent_user and "FortressOS login: " in chunk:
                        sent_user = True
                        send_str("operator\n")
                    if sent_user and not sent_pass and "Password: " in chunk:
                        sent_pass = True
                        send_str(TEST_PASSWORD + "\n")
                    if require_newline:
                        nl = chunk.find("\n")
                        if nl != -1 and re.search(r"(?:" + PROMPT_PATTERN + r")\s*\Z", chunk[nl+1:]):
                            return txt
                    else:
                        if re.search(r"(?:" + PROMPT_PATTERN + r")\s*\Z", chunk):
                            return txt
                    assert proc.poll() is None, f"QEMU exited prematurely: code={proc.poll()}"
                    time.sleep(0.05)
                full = get_text()
                raise AssertionError(f"Timeout waiting for prompt. Tail:\n{full[-1000:]}")

            def exec_cmd(cmd_str, timeout=20):
                pos = len(get_text())
                send_str(cmd_str + "\n")
                wait_for_prompt(pos, timeout=timeout, require_newline=True)
                return get_text()[pos:]

            print("  [4] Booting FortressOS and awaiting operator login...", flush=True)
            wait_for_prompt(0, timeout=60)
            assert "[EXT4 JOURNAL] PASS boot 1" in get_text(), f"Missing journal mount:\n{get_text()[-1000:]}"
            print("  [4] Operator console session ready with ext4 journaled /mnt.", flush=True)

            print("  [5] Setting up ~/.ssh/authorized_keys...", flush=True)
            exec_cmd("mkdir .ssh")
            exec_cmd(f"echo '{client_pub_line.strip()}' > .ssh/authorized_keys")
            exec_cmd("chmod 700 .ssh")
            exec_cmd("chmod 600 .ssh/authorized_keys")
            out_ls = exec_cmd("ls -l .ssh")
            assert "authorized_keys" in out_ls, f"authorized_keys not found: {out_ls}"

            # Elevate to root to run disk staging workload
            print("  [5b] Elevating to root shell on console...", flush=True)
            pos = len(get_text())
            send_str("sudo /bin/shell\n")
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                chunk = get_text()[pos:]
                if "sudo password: " in chunk:
                    break
                time.sleep(0.05)
            assert "sudo password: " in get_text()[pos:], f"Missing sudo password prompt:\n{get_text()[pos:]}"
            send_str(f"{TEST_PASSWORD}\n")
            wait_for_prompt(pos, timeout=10, require_newline=True)
            print("  [5b] Root shell established on console.", flush=True)

            # Check if sshd is already running (e.g. started by watchtower)
            print("  [6] Checking /sbin/sshd daemon availability on port 22...", flush=True)
            sshd_ready = False
            for _ in range(10):
                try:
                    with socket.create_connection(("127.0.0.1", host_port), timeout=1.0) as s:
                        banner = s.recv(256)
                        if banner.startswith(b"SSH-2.0-"):
                            sshd_ready = True
                            print("  [6] /sbin/sshd is active and listening on port 22.", flush=True)
                            break
                except (socket.timeout, ConnectionRefusedError, OSError):
                    time.sleep(0.5)

            if not sshd_ready:
                print("  [6] Launching /sbin/sshd daemon on console...", flush=True)
                exec_cmd("/sbin/sshd &")
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    if "Listening on port 22" in get_text():
                        sshd_ready = True
                        break
                    time.sleep(0.1)
                assert sshd_ready, f"sshd failed to start:\n{get_text()[-1000:]}"
                print("  [6] /sbin/sshd listening on port 22.", flush=True)

            # Retrieve host key via ssh-keyscan
            print("  [6b] Scanning host key via ssh-keyscan...", flush=True)
            scan_out = subprocess.run(
                ["ssh-keyscan", "-vvv", "-t", "ed25519", "-p", str(host_port), "127.0.0.1"],
                capture_output=True, text=True, timeout=10
            )
            scanned_pub = scan_out.stdout.strip()
            assert scanned_pub, f"ssh-keyscan failed: {scan_out.stderr}"
            known_hosts = tmp_dir / "known_hosts"
            known_hosts.write_text(scanned_pub + "\n")
            print("  [6b] Host key acquired and stored in known_hosts.", flush=True)

            # Establish interactive OpenSSH PTY session
            print("  [7] Establishing interactive OpenSSH PTY session...", flush=True)
            client_err = tempfile.TemporaryFile()
            ssh_client = subprocess.Popen([
                "ssh", "-vvv", "-tt", "-o", "StrictHostKeyChecking=yes",
                "-o", f"UserKnownHostsFile={known_hosts}", "-o", "ConnectTimeout=10",
                "-o", "BatchMode=yes", "-i", str(client_key),
                "-p", str(host_port), "operator@127.0.0.1"
            ], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=client_err)

            ssh_buf = b""
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                ready, _, _ = select.select([ssh_client.stdout], [], [], 0.1)
                if ready:
                    c = os.read(ssh_client.stdout.fileno(), 1024)
                    if c:
                        ssh_buf += c
                        clean = re.sub(rb"\x1b\[[0-9;?]*[ -/]*[@-~]", b"", ssh_buf).replace(b"\r", b"")
                        if clean.endswith(b"$ "):
                            break
            if not (ssh_buf.endswith(b"$ ") or b"$ " in ssh_buf):
                client_err.seek(0)
                err_text = client_err.read().decode(errors="replace")
                serial_tail = get_text()[-1500:]
                raise AssertionError(f"Missing SSH prompt (ssh_buf={ssh_buf!r})\nSSH STDERR:\n{err_text}\nSERIAL LOG:\n{serial_tail}")
            print("  [7] Interactive SSH PTY session active.", flush=True)

            # Phase 1: Measure Idle Keystroke SLA
            print("  [8] Measuring Baseline (Idle) SSH Keystroke SLA...", flush=True)
            idle_sample_text = "echo 'FORTRESS_IDLE_KEYSTROKE_MEASUREMENT_RUN_0123456789'"
            idle_latencies = measure_keystroke_stream(ssh_client, idle_sample_text, delay=0.02)
            idle_cmd_ms, _ = measure_command_roundtrip(ssh_client, "", "FORTRESS_IDLE_KEYSTROKE_MEASUREMENT_RUN_0123456789")
            idle_stats = compute_stats(idle_latencies)
            print(f"      Idle Keystrokes (N={idle_stats['count']}): Mean={idle_stats['mean_ms']}ms, Median={idle_stats['median_ms']}ms, p95={idle_stats['p95_ms']}ms, Max={idle_stats['max_ms']}ms | Cmd={idle_cmd_ms:.1f}ms", flush=True)

            # Phase 2: Start background 32-block staging workload on console (running as root)
            print("  [9] Starting active 32-block staging workload in background (/bin/stage-probe &)...", flush=True)
            pos_stage = len(get_text())
            send_str("/bin/stage-probe &\n")
            time.sleep(1.0)

            # Phase 3: Measure Active Staging Keystroke SLA
            print("  [10] Measuring Active Staging SSH Keystroke SLA (during concurrent 32-block ext4 writes)...", flush=True)
            active_text = "echo 'FORTRESS_ACTIVE_STAGING_KEYSTROKE_MEASUREMENT_0123456789abcdefghijklmnopqrstuvwxyz_ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789_SLA_GATE_CHECK'"
            active_latencies = measure_keystroke_stream(ssh_client, active_text, delay=0.025)
            active_cmd_ms, _ = measure_command_roundtrip(ssh_client, "", "FORTRESS_ACTIVE_STAGING_KEYSTROKE_MEASUREMENT")
            active_stats = compute_stats(active_latencies)
            print(f"      Active Staging Keystrokes (N={active_stats['count']}): Mean={active_stats['mean_ms']}ms, Median={active_stats['median_ms']}ms, p95={active_stats['p95_ms']}ms, Max={active_stats['max_ms']}ms | Cmd={active_cmd_ms:.1f}ms", flush=True)

            # Wait for stage-probe completion on console
            print("  [11] Waiting for /bin/stage-probe to complete on console...", flush=True)
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                txt = get_text()
                if "STAGE Ring3 PASS" in txt[pos_stage:]:
                    break
                assert proc.poll() is None, "QEMU died while running stage-probe"
                time.sleep(0.5)
            assert "STAGE Ring3 PASS" in get_text()[pos_stage:], f"stage-probe failed or stalled:\n{get_text()[-1500:]}"
            print("  [11] /bin/stage-probe completed: STAGE Ring3 PASS.", flush=True)

            ssh_client.stdin.write(b"exit\r")
            ssh_client.stdin.flush()
            ssh_client.wait(timeout=5)

            # Verification of Acceptance Criteria
            print("\n  --- SLA EVALUATION RESULTS ---", flush=True)
            print(f"  Idle Keystroke p95:          {idle_stats['p95_ms']:.2f} ms")
            print(f"  Active Staging Keystroke p95: {active_stats['p95_ms']:.2f} ms  (Target: < 100 ms)")
            print(f"  Idle Command Latency:        {idle_cmd_ms:.2f} ms")
            print(f"  Active Command Latency:      {active_cmd_ms:.2f} ms  (Target: < 500 ms)")

            assert active_stats['p95_ms'] < 100.0, f"SLA Gate FAILED: Active Keystroke p95 {active_stats['p95_ms']} ms exceeds 100 ms!"
            assert active_cmd_ms < 500.0, f"SLA Gate FAILED: Active Command Latency {active_cmd_ms} ms exceeds 500 ms!"
            print(f"  >>> [PASS] All Interactive SSH SLA Criteria Met Under Active 32-Block Staging! <<<\n", flush=True)

            result = {
                "firmware": firmware,
                "smp": smp,
                "result": "PASS",
                "idle_keystrokes": idle_stats,
                "active_keystrokes": active_stats,
                "idle_command_ms": round(idle_cmd_ms, 2),
                "active_command_ms": round(active_cmd_ms, 2)
            }
            return result

        finally:
            stop_drain.set()
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)


def main():
    import argparse
    parser = argparse.ArgumentParser(description="Interactive SSH SLA Gate Test")
    parser.add_argument("--smp", type=int, default=4, help="SMP core count (default: 4)")
    parser.add_argument("--firmware", choices=["bios", "uefi"], default="bios", help="Firmware type")
    args = parser.parse_args()

    res = run_sla_test(firmware=args.firmware, smp=args.smp)
    out_dir = REPO / ".codex-remote-attachments" / f"ssh-sla-{time.strftime('%Y%m%d-%H%M%S')}"
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "result.json").write_text(json.dumps(res, indent=2) + "\n")
    print(f"Evidence recorded in: {out_dir}")


if __name__ == "__main__":
    main()
