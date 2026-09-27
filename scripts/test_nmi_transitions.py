#!/usr/bin/env python3
"""Inject actual QEMU NMIs at unmodified syscall instruction boundaries.

Uses QEMU's GDB remote protocol for hardware breakpoints/register inspection,
and QMP inject-nmi for delivery. No guest INT 2, inserted waits, or guest memory
writes. Firmware-declared LAPIC NMI routing is configured by normal kernel boot.
Single-stepping verifies NMI/IRET returns; it is never used to deliver an NMI.
The original SYSRET suite and the disposable sigreturn fixture boot separately.
"""
import json
import re
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tarfile
import tempfile
import time

REPO = Path(__file__).resolve().parent.parent
PROBES = (
    "syscall_entry_stub",             # Ring 0, user RSP, before saving it
    "syscall_entry_kernel_gs",        # after SWAPGS, before saving user RSP
    "syscall_entry_rsp_saved",        # Ring 0, saved user RSP, before stack switch
    "syscall_entry_kernel_rsp",       # immediately after kernel stack switch
    "syscall_exit_restore_rsp",       # immediately before restoring user RSP
    "syscall_exit_kernel_gs",         # user RSP, before exit SWAPGS
    "syscall_exit_user_rsp",          # Ring 0, user RSP, immediately before SYSRET
)
ROUNDS = 4
SIGRETURN_PROBES = (
    "sigreturn_restore_regs", "sigreturn_before_swapgs",
    "sigreturn_after_swapgs", "sigreturn_before_iretq",
)
# interrupt_frame_t / signal_frame_v1_t GPR order -> QEMU register order.
FRAME_REGS = (0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15)
RFLAGS_ALLOWED = 0x240CD5
RESTORER = 0x7FFFEFFFE000


def symbols(elf="bin/fortress.elf"):
    result = {}
    for line in subprocess.check_output(["nm", "-an", elf], cwd=REPO, text=True).splitlines():
        fields = line.split()
        if len(fields) == 3:
            result[fields[2]] = int(fields[0], 16)
    if elf != "bin/fortress.elf":
        return result
    for name in (*PROBES, "isr2", "isr_return_iretq", "ist2_memory", "cpu_locals"):
        assert name in result, f"Missing probe/debug symbol: {name}"
    result["g_syscall_scratch_rsp"] = result["cpu_locals"] + 8
    result["g_tss_rsp0"] = result["cpu_locals"] + 16
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


def gs_base(qmp):
    # HMP exposes the actual segment-cache base, not the GS selector or an
    # inferred SWAPGS state. Fail closed if QEMU changes its output format.
    registers = qmp.execute("human-monitor-command", {"command-line": "info registers"})
    match = re.search(r"^GS\s*=\s*[0-9a-fA-F]+\s+([0-9a-fA-F]{16})\b", registers, re.M)
    assert match, f"QEMU did not expose GS base: {registers}"
    return int(match[1], 16)


def step(remote):
    assert remote.request("s").startswith(("T05", "S05"))


def make_signal_iso(temp):
    """Replace only the shell in a disposable copy; keep production untouched."""
    root = temp / "iso"
    shutil.copytree(REPO / "build/iso_root", root)
    archive = temp / "initramfs.tar"
    with tarfile.open(REPO / "bin/initramfs.tar") as src, \
            tarfile.open(archive, "w", format=tarfile.USTAR_FORMAT) as dst:
        for member in src.getmembers():
            if member.name.lstrip("./") != "bin/shell":
                dst.addfile(member, src.extractfile(member) if member.isfile() else None)
        dst.add(REPO / "build/s8_nmi_user.elf", arcname="bin/shell")
    for path in (root / "boot/initramfs.tar", root / "initramfs.tar"):
        shutil.copyfile(archive, path)
    config = ("timeout: 0\n/FortressOS sigreturn NMI fixture\n    protocol: limine\n"
              "    kernel_path: boot():/boot/fortress.elf\n"
              "    module_path: boot():/boot/initramfs.tar\n")
    for path in (root / "limine.conf", root / "boot/limine.conf", root / "boot/limine/limine.conf"):
        path.write_text(config)
    iso = temp / "nmi-signals.iso"
    subprocess.run(["xorriso", "-as", "mkisofs", "-b", "boot/limine/limine-bios-cd.bin",
                    "-no-emul-boot", "-boot-load-size", "4", "-boot-info-table",
                    "--efi-boot", "boot/limine/limine-uefi-cd.bin", "-efi-boot-part",
                    "--efi-boot-image", "--protective-msdos-label", str(root), "-o", str(iso)],
                   check=True, timeout=60, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run([str(REPO / "limine/limine"), "bios-install", str(iso)],
                   check=True, timeout=30, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return iso


def signal_context(remote, qmp, sym, user, origin, expected_frame_id):
    # Observe the actual interrupt frame BEFORE it is copied to user memory.
    # This checks origin independently of the frame the restorer later supplies.
    remote.resume_to(sym["signal_deliver_handler"])
    regs, _, cs, _ = remote.registers()
    assert cs == 8
    original = struct.unpack("<22Q", remote.memory(regs[5], 176))  # RDI
    assert original[18] == 0x23 and original[21] == 0x1b
    assert original[15] == (0x80 if origin == "syscall" else 0x20), "Wrong delivery origin"
    if origin == "syscall":
        assert original[17] == user["nmi_self_resume"] and original[0] == 0
        assert original[2] == original[17], "Saved syscall RCX must be hardware return RIP"
        for index in (1, 3, 6, 7, 8, 9, 11, 12, 13, 14):
            assert original[index] == 0x101 + index, "Syscall GPR sentinel lost"
    else:
        assert user["nmi_timer_loop"] <= original[17] < user["nmi_timer_end"]
        assert original[:15] == tuple(range(0x101, 0x110)), "Timer GPR sentinels lost"
    assert original[19] & 0x400, "Fixture must exercise saved DF"

    remote.resume_to(user["nmi_handler"])
    regs, flags, cs, ss = remote.registers()
    assert (cs, ss) == (0x23, 0x1b) and regs[5] == 15
    assert not flags & 0x400 and flags & 0x200, "Handler DF/IF incorrect"
    user_gs = gs_base(qmp)
    assert user_gs < 0x800000000000
    h = regs[7]
    assert h % 16 == 8
    assert int.from_bytes(remote.memory(h, 8), "little") == RESTORER
    sf = struct.unpack("<28Q", remote.memory(h + 8, 224))
    assert sf[:2] == (1, 224), f"Invalid signal frame version/size: {sf[:2]}"
    # Generations start at zero per process; zero is a valid active-frame ID.
    assert sf[2] == expected_frame_id, (
        f"{origin} frame_id: expected {expected_frame_id}, got {sf[2]}")
    assert sf[3] == 0, f"Nonzero signal frame reserved field: {sf[3]:#x}"
    assert sf[4:19] == original[:15], "Signal frame changed saved GPRs"
    assert sf[19:24] == (original[19], original[17], original[20], 0x23, 0x1b)
    assert sf[24:] == (0, 15, 0, 0), "Unexpected mask/signal/reserved fields"

    # Prove handler RET reaches the real mapped stub, with F as RSP.
    remote.resume_to(RESTORER)
    assert remote.registers()[0][7] == h + 8 and remote.registers()[2] == 0x23
    stub = remote.memory(sym["sigrestorer_start"], sym["sigrestorer_end"] - sym["sigrestorer_start"])
    assert remote.memory(RESTORER, len(stub)) == stub
    remote.resume_to(sym["sigreturn_restore_regs"])
    regs, _, cs, _ = remote.registers()
    assert cs == 8 and regs[7] >= 0xffff800000000000
    committed = struct.unpack("<22Q", remote.memory(regs[7], 176))
    expected_flags = (original[19] & RFLAGS_ALLOWED) | 0x202
    expected_iret = (original[17], 0x23, expected_flags, original[20], 0x1b)
    assert committed[:15] == original[:15], "Sigreturn changed GPRs before pops"
    assert committed[17:] == expected_iret, "Sigreturn committed wrong IRET frame"
    return original, expected_iret, regs[7], user_gs


def signal_probe(remote, qmp, sym, user, name, origin, inject, expected_frame_id):
    original, expected_iret, frame_base, user_gs = signal_context(
        remote, qmp, sym, user, origin, expected_frame_id)
    address = sym[name]
    if address != sym["sigreturn_restore_regs"]:
        remote.resume_to(address)
    before, flags, cs, ss = remote.registers()
    assert cs == 8 and not flags & 0x200
    assert before[7] == frame_base + (0 if name == "sigreturn_restore_regs" else 136)
    expected_gs = user_gs if address == sym["sigreturn_after_swapgs"] else sym["cpu_locals"]
    assert gs_base(qmp) == expected_gs, "Wrong GS base at sigreturn boundary"
    # Retain all 176 bytes, including already-popped GPRs and vector/error;
    # IST2 must not touch any part of this interrupted kernel stack frame.
    kernel_stack = remote.memory(frame_base, 176)
    user_stack = remote.memory(original[20] - 256, 256)
    scratch = remote.memory(sym["g_syscall_scratch_rsp"], 8)
    rsp0 = remote.memory(sym["g_tss_rsp0"], 8)
    entered = None
    if inject:
        qmp.execute("inject-nmi")
        remote.resume_to(sym["isr2"])
        entered, _, entered_cs, _ = remote.registers()
        bottom = sym["ist2_memory"] + 4096
        assert entered_cs == 8 and bottom <= entered[7] <= bottom + 16384 - 40
        assert struct.unpack("<5Q", remote.memory(entered[7], 40)) == (
            address, cs, flags, before[7], ss), "NMI missed exact sigreturn boundary"
        assert gs_base(qmp) == expected_gs
        remote.resume_to(sym["isr_return_iretq"])
        restored = remote.registers()[0]
        assert restored[:7] == before[:7] and restored[8:16] == before[8:16]
        assert gs_base(qmp) == expected_gs, "NMI failed to restore actual GS base"
        step(remote)
        assert remote.registers() == (before, flags, cs, ss)
        assert gs_base(qmp) == expected_gs
        assert remote.memory(frame_base, 176) == kernel_stack, "NMI changed kernel stack/IRET frame"
        assert remote.memory(original[20] - 256, 256) == user_stack, "NMI changed user stack"
        assert remote.memory(sym["g_syscall_scratch_rsp"], 8) == scratch
        assert remote.memory(sym["g_tss_rsp0"], 8) == rsp0

    # Stepping only after the NMI (or an explicitly non-injected alias case).
    # Never execute user code before comparing all restored GPRs/RIP/RSP.
    for _ in range(20):
        if remote.registers()[0][16] == sym["sigreturn_before_swapgs"]:
            assert gs_base(qmp) == sym["cpu_locals"]
        if remote.registers()[0][16] == sym["sigreturn_before_iretq"]:
            break
        step(remote)
    else:
        raise AssertionError("Sigreturn failed to reach IRETQ")
    regs, _, cs, _ = remote.registers()
    assert cs == 8 and regs[7] == frame_base + 136 and gs_base(qmp) == user_gs
    assert struct.unpack("<5Q", remote.memory(regs[7], 40)) == expected_iret
    assert tuple(regs[i] for i in FRAME_REGS) == original[:15]
    step(remote)
    regs, user_flags, cs, ss = remote.registers()
    assert (cs, ss) == (0x23, 0x1b)
    assert (regs[16], regs[7], user_flags) == (original[17], original[20], expected_iret[2])
    assert tuple(regs[i] for i in FRAME_REGS) == original[:15], "IRETQ lost GPRs (including RAX/RCX/R11)"
    assert gs_base(qmp) == user_gs
    return {"probe": name, "origin": origin, "rip": hex(address), "injected": inject,
            "frame_id": expected_frame_id,
            "nmi_rsp": hex(entered[7]) if entered else None,
            "gs_base": hex(expected_gs), "user_gs_base": hex(user_gs),
            "saved_vector": original[15], "user_rip": hex(regs[16]), "user_rsp": hex(regs[7]),
            "user_rflags": hex(user_flags), "gprs": [hex(x) for x in original[:15]],
            "kernel_frame_unchanged": True, "full_context_restored": True}


def check_test_recovery(remote, qmp, sym):
    """Observe the legacy INT 0x80 test's armed Ring 0 recovery return.

    Scheduled fast-syscall processes exit through process_exit, so waiting for
    syscall_return_iretq cannot exercise recovery in this boot. Match the real
    armed target at the common ISR return instead; ordinary user returns must
    not satisfy this check.
    """
    remote.resume_to(sym["syscall_set_recovery"])
    armed, _, cs, _ = remote.registers()
    target, stack = armed[5], armed[4]  # RDI/RSi: recovery RIP/RSP
    assert cs == 8 and target >= 0xffff800000000000 and stack >= 0xffff800000000000
    for _ in range(256):
        remote.resume_to(sym["isr_return_iretq"])
        before, _, cs, _ = remote.registers()
        frame = struct.unpack("<5Q", remote.memory(before[7], 40))
        if frame[0] == target:
            break
        # Move past the current hardware breakpoint before arming it again.
        step(remote)
    else:
        raise AssertionError("Legacy syscall test did not return to its armed recovery target")
    assert frame[3] == stack, "Recovery changed the armed kernel stack"
    assert cs == 8 and frame[1] == 8 and frame[4] == 0x10
    assert frame[0] >= 0xffff800000000000 and frame[3] >= 0xffff800000000000
    assert frame[2] == 2 and gs_base(qmp) == sym["cpu_locals"]
    step(remote)
    after, flags, cs, ss = remote.registers()
    assert (after[16], cs, flags, after[7], ss) == frame
    assert after[:7] == before[:7] and after[8:16] == before[8:16]
    assert gs_base(qmp) == sym["cpu_locals"], "Kernel test recovery swapped to user GS"
    return {"kernel_recovery": True, "path": "int80_isr_return_iretq",
            "gs_base": hex(sym["cpu_locals"]),
            "rip": hex(after[16]), "rsp": hex(after[7])}


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
    if name in ("syscall_entry_stub", "syscall_entry_kernel_gs", "syscall_entry_rsp_saved",
                "syscall_exit_kernel_gs", "syscall_exit_user_rsp"):
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


def run(mode, sym, signals=False):
    prefix = "nmi-sigreturn" if signals else "nmi"
    log = REPO / "build" / f"{prefix}-{mode}.log"
    report = []
    with tempfile.TemporaryDirectory(prefix="fortress-nmi-") as temp:
        temp = Path(temp)
        iso = make_signal_iso(temp) if signals else REPO / "bin/fortress.iso"
        cmd = ["qemu-system-x86_64", "-accel", "tcg", "-smp", "1", "-M", "q35", "-m", "2G",
               "-display", "none", "-serial", f"file:{log}", "-monitor", "none", "-no-reboot",
               "-S", "-chardev", f"socket,path={temp}/gdb,server=on,wait=off,id=gdb0",
               "-gdb", "chardev:gdb0", "-qmp", f"unix:{temp}/qmp,server=on,wait=off",
               "-drive", "file=build/nvme_gpt.img,if=none,id=nvm0,format=raw,snapshot=on",
               "-device", "nvme,serial=fortress0,drive=nvm0", "-boot", "d", "-cdrom", str(iso)]
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
            if signals:
                user = symbols("build/s8_nmi_user.elf")
                for name in (*SIGRETURN_PROBES, "signal_deliver_handler", "sigrestorer_start", "sigrestorer_end",
                             "syscall_set_recovery"):
                    assert name in sym, f"Missing sigreturn symbol: {name}"
                recovery = check_test_recovery(remote, qmp, sym)
                print(f"PASS {mode}: separate kernel test-recovery IRET/GS check", flush=True)
                # Only the documented after-SWAPGS/before-IRET alias is legal.
                groups = {}
                for name in SIGRETURN_PROBES:
                    groups.setdefault(sym[name], []).append(name)
                for names in groups.values():
                    assert len(names) == 1 or names == ["sigreturn_after_swapgs", "sigreturn_before_iretq"], names
                for cycle in range(ROUNDS):
                    for index, name in enumerate(SIGRETURN_PROBES):
                        aliases = groups[sym[name]]
                        for origin in ("syscall", "timer"):
                            # Parent persists across cases; each timer child is
                            # a fresh process and catches exactly one signal.
                            frame_id = cycle * len(SIGRETURN_PROBES) + index if origin == "syscall" else 0
                            result = signal_probe(remote, qmp, sym, user, name, origin,
                                                  name == aliases[0], frame_id)
                            result.update(round=cycle + 1, aliases=aliases)
                            report.append(result)
                            kind = "exact NMI" if result["injected"] else "alias roundtrip (not counted)"
                            print(f"PASS {mode} round {cycle + 1}: {origin} {name}: {kind}", flush=True)
                expected_nmis = ROUNDS * len(groups) * 2
                assert sum(r["injected"] for r in report) == expected_nmis
                # Per-origin/per-address coverage, rather than an aggregate count.
                for origin in ("syscall", "timer"):
                    for address in groups:
                        assert sum(r["injected"] and r["origin"] == origin and r["rip"] == hex(address)
                                   for r in report) == ROUNDS
            else:
                for cycle in range(ROUNDS):
                    for name in PROBES:
                        result = probe(remote, qmp, sym, name)
                        result["round"] = cycle + 1
                        report.append(result)
                        print(f"PASS {mode} round {cycle + 1}: {name}, exact NMI RIP, IST2 and return", flush=True)
                expected_nmis = ROUNDS * len(PROBES)
            assert remote.request("D") == "OK"
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                output = log.read_text(errors="replace")
                assert "S8 NMI FIXTURE FAIL" not in output, output[-2000:]
                marker = "S8 NMI FIXTURE PASS" if signals else "[BOOT] FortressOS Phase 9 (Step 9C.2) complete."
                if marker in output:
                    break
                if child.poll() is not None:
                    raise RuntimeError("QEMU exited before acceptance suite completed")
                time.sleep(0.2)
            else:
                raise RuntimeError(f"Post-NMI acceptance suite timed out: {log}")
            assert output.count("[NMI] Non-Maskable Interrupt received on IST2!") == expected_nmis
            evidence = {"test_recovery": recovery, "boundaries": report} if signals else report
            (REPO / "build" / f"{prefix}-{mode}.json").write_text(json.dumps(evidence, indent=2) + "\n")
            print(f"PASS {mode} {prefix}: {expected_nmis} exact-boundary NMIs; acceptance completed", flush=True)
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
        run(firmware, symbols(), signals=True)
