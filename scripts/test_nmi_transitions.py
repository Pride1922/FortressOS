#!/usr/bin/env python3
"""Inject actual QEMU NMIs at unmodified syscall instruction boundaries.

Uses QEMU's GDB remote protocol for hardware breakpoints/register inspection,
and QMP inject-nmi for delivery. No guest INT 2, inserted waits, or guest memory
writes. Firmware-declared LAPIC NMI routing is configured by normal kernel boot.
Single-stepping is used only AFTER the NMI handler.
"""
import json
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import time

REPO = Path(__file__).resolve().parent.parent
PROBES = (
    "syscall_entry_stub",             # Ring 0, user RSP, before saving it
    "syscall_entry_rsp_saved",        # Ring 0, saved user RSP, before stack switch
    "syscall_entry_kernel_rsp",       # immediately after kernel stack switch
    "syscall_exit_restore_rsp",       # immediately before restoring user RSP
    "syscall_exit_user_rsp",          # Ring 0, user RSP, immediately before SYSRET
)
ROUNDS = 4


def symbols():
    result = {}
    for line in subprocess.check_output(["nm", "-an", "bin/fortress.elf"], cwd=REPO, text=True).splitlines():
        fields = line.split()
        if len(fields) == 3:
            result[fields[2]] = int(fields[0], 16)
    for name in (*PROBES, "isr2", "isr_return_iretq", "ist2_memory", "g_tss_rsp0", "g_syscall_scratch_rsp"):
        assert name in result, f"Missing probe/debug symbol: {name}"
    return result


def connect(path):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        sock = socket.socket(socket.AF_UNIX)
        try:
            sock.connect(str(path))
            sock.settimeout(45)
            return sock
        except (FileNotFoundError, ConnectionRefusedError):
            sock.close()
            time.sleep(0.05)
    raise RuntimeError(f"QEMU socket unavailable: {path}")


class Remote:
    def __init__(self, path):
        self.sock = connect(path)

    def byte(self):
        b = self.sock.recv(1)
        if not b:
            raise RuntimeError("QEMU debugger disconnected")
        return b

    def request(self, message):
        data = message.encode()
        self.sock.sendall(b"$" + data + b"#" + f"{sum(data) & 255:02x}".encode())
        while self.byte() != b"$":
            pass  # packet acknowledgement
        encoded = bytearray()
        while True:
            b = self.byte()
            if b == b"#":
                break
            encoded += b
        checksum = self.byte() + self.byte()
        assert sum(encoded) & 255 == int(checksum, 16), "GDB packet checksum"
        self.sock.sendall(b"+")
        decoded = bytearray()
        i = 0
        while i < len(encoded):
            b = encoded[i]
            if b == ord("}"):
                i += 1
                decoded.append(encoded[i] ^ 0x20)
            elif b == ord("*"):
                i += 1
                decoded.extend(bytes([decoded[-1]]) * (encoded[i] - 29))
            else:
                decoded.append(b)
            i += 1
        return decoded.decode()

    def memory(self, addr, size):
        data = self.request(f"m{addr:x},{size:x}")
        assert not data.startswith("E"), f"Unmapped debugger read {addr:#x}: {data}"
        result = bytes.fromhex(data)
        assert len(result) == size
        return result

    def registers(self):
        raw = bytes.fromhex(self.request("g"))
        # QEMU x86-64 core register order: 16 GPRs, RIP, EFLAGS, CS, SS, DS...
        assert len(raw) >= 148
        gprs = list(struct.unpack_from("<17Q", raw))
        flags, cs, ss = struct.unpack_from("<III", raw, 136)
        return gprs, flags, cs, ss

    def breakpoint(self, addr, enable=True):
        assert self.request(f"{'Z' if enable else 'z'}1,{addr:x},1") == "OK"

    def resume_to(self, addr):
        self.breakpoint(addr)
        response = self.request("c")
        assert response.startswith(("T05", "S05")), response
        assert self.registers()[0][16] == addr, "Unexpected debugger stop"
        self.breakpoint(addr, False)


class QMP:
    def __init__(self, path):
        self.sock = connect(path)
        self.stream = self.sock.makefile("rb")
        assert "QMP" in json.loads(self.stream.readline())
        self.execute("qmp_capabilities")

    def execute(self, command, arguments=None):
        request = {"execute": command}
        if arguments is not None:
            request["arguments"] = arguments
        self.sock.sendall(json.dumps(request).encode() + b"\n")
        while True:
            reply = json.loads(self.stream.readline())
            if "event" in reply:
                continue
            assert "return" in reply, reply
            return reply["return"]


def probe(remote, qmp, sym, name):
    address = sym[name]
    remote.resume_to(address)
    before, flags, cs, ss = remote.registers()
    assert cs == 8 and not flags & 0x200, "Must inject at CPL0 with IF=0"
    scratch = remote.memory(sym["g_syscall_scratch_rsp"], 8)
    rsp0 = remote.memory(sym["g_tss_rsp0"], 8)
    user_rsp = before[7] if before[7] < 0x800000000000 else int.from_bytes(scratch, "little")
    assert 0x1000 <= user_rsp < 0x800000000000
    user_stack = remote.memory(user_rsp - 128, 128)
    if name in ("syscall_entry_stub", "syscall_entry_rsp_saved", "syscall_exit_user_rsp"):
        assert before[7] == user_rsp, "Expected vulnerable user-RSP window"
    else:
        assert before[7] >= 0xffff800000000000, "Expected kernel RSP"

    # Inject while paused, then CONTINUE (not single-step, which masks IRQs by
    # default). Assert the hardware NMI frame saved the exact target RIP.
    qmp.execute("inject-nmi")
    remote.resume_to(sym["isr2"])
    entered, _, entered_cs, _ = remote.registers()
    ist_bottom = sym["ist2_memory"] + 4096
    ist_top = ist_bottom + 16384
    assert entered_cs == 8 and ist_bottom <= entered[7] <= ist_top - 40
    saved_rip, saved_cs, saved_flags, saved_rsp, saved_ss = struct.unpack(
        "<5Q", remote.memory(entered[7], 40))
    assert (saved_rip, saved_cs, saved_flags, saved_rsp, saved_ss) == (
        address, cs, flags, before[7], ss), "NMI delivered outside requested boundary"

    remote.resume_to(sym["isr_return_iretq"])
    restored_gprs = remote.registers()[0]
    assert restored_gprs[:7] == before[:7] and restored_gprs[8:16] == before[8:16], "NMI clobbered GPRs"
    assert remote.memory(sym["g_syscall_scratch_rsp"], 8) == scratch
    assert remote.memory(sym["g_tss_rsp0"], 8) == rsp0
    assert remote.memory(user_rsp - 128, 128) == user_stack, "NMI wrote to user stack"
    assert remote.request("s").startswith(("T05", "S05"))
    after = remote.registers()
    assert after == (before, flags, cs, ss), "IRET did not restore interrupted state"
    if name == "syscall_exit_user_rsp":
        assert remote.request("s").startswith(("T05", "S05"))
        user, user_flags, user_cs, user_ss = remote.registers()
        assert user_cs == 0x23 and user_ss == 0x1b
        assert user[16] == before[2] and user[7] == user_rsp
        assert user_flags & 0x200, "SYSRET failed to restore user interrupts"
    return {"probe": name, "rip": hex(address), "interrupted_rsp": hex(before[7]),
            "nmi_rsp": hex(entered[7]), "ist2": True, "registers_restored": True,
            "user_stack_unchanged": True}


def run(mode, sym):
    log = REPO / "build" / f"nmi-{mode}.log"
    report = []
    with tempfile.TemporaryDirectory(prefix="fortress-nmi-") as temp:
        temp = Path(temp)
        cmd = ["qemu-system-x86_64", "-accel", "tcg", "-smp", "1", "-M", "q35", "-m", "2G",
               "-display", "none", "-serial", f"file:{log}", "-monitor", "none", "-no-reboot",
               "-S", "-chardev", f"socket,path={temp}/gdb,server=on,wait=off,id=gdb0",
               "-gdb", "chardev:gdb0", "-qmp", f"unix:{temp}/qmp,server=on,wait=off",
               "-drive", "file=build/nvme_gpt.img,if=none,id=nvm0,format=raw,snapshot=on",
               "-device", "nvme,serial=fortress0,drive=nvm0", "-boot", "d", "-cdrom", "bin/fortress.iso"]
        if mode == "uefi":
            shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", temp / "vars.fd")
            cmd += ["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
                    "-drive", f"if=pflash,format=raw,unit=1,file={temp}/vars.fd"]
        log.write_text("")
        child = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        remote = qmp = None
        try:
            qmp = QMP(temp / "qmp")
            remote = Remote(temp / "gdb")
            remote.request("qSupported")
            # Force long-register layout before inspecting 64-bit kernel state.
            remote.request("qXfer:features:read:target.xml:0,fff")
            for cycle in range(ROUNDS):
                for name in PROBES:
                    result = probe(remote, qmp, sym, name)
                    result["round"] = cycle + 1
                    report.append(result)
                    print(f"PASS {mode} round {cycle + 1}: {name}, exact NMI RIP, IST2 and return", flush=True)
            assert remote.request("D") == "OK"
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                output = log.read_text(errors="replace")
                if "[BOOT] FortressOS Phase 9 (Step 9C.2) complete." in output:
                    break
                if child.poll() is not None:
                    raise RuntimeError("QEMU exited before acceptance suite completed")
                time.sleep(0.2)
            else:
                raise RuntimeError(f"Post-NMI acceptance suite timed out: {log}")
            assert output.count("[NMI] Non-Maskable Interrupt received on IST2!") == ROUNDS * len(PROBES)
            (REPO / "build" / f"nmi-{mode}.json").write_text(json.dumps(report, indent=2) + "\n")
            print(f"PASS {mode}: {len(report)} exact-boundary NMIs; complete boot suite passed", flush=True)
        finally:
            if remote:
                remote.sock.close()
            if qmp:
                qmp.stream.close()
                qmp.sock.close()
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
            if child.stderr:
                child.stderr.close()


if __name__ == "__main__":
    for firmware in ("bios", "uefi"):
        run(firmware, symbols())
