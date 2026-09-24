#!/usr/bin/env python3
"""Piece 2: read-only GDB inspection, real AP #DF, and QMP hardware NMIs.
BIOS/UEFI, 1/4/8 CPUs, snapshot NVMe fixture. No guest debugger writes.
Exact syscall-boundary NMI coverage remains the separate BSP test-nmi suite.
"""
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import time
from test_nmi_transitions import REPO, Remote, QMP, symbols


def layout(temp):
    fields = ("self", "rsp0", "current_thread", "id", "lapic_id", "online",
              "probe", "probe_rsp", "preempt_count", "irq_depth", "nmi_count", "nmi_rsp")
    source = temp / "layout.c"
    source.write_text('#include <stdio.h>\n#include "percpu.h"\nint main(void) {\n'
                      'printf("%zu ", sizeof(cpu_local_t));\n' + ''.join(
                          f'printf("%zu ", __builtin_offsetof(cpu_local_t, {f}));\n' for f in fields)
                      + 'return 0; }\n')
    subprocess.run(["gcc", "-Isrc/include", "-Isrc/drivers", "-Isrc/arch/x86_64",
                    str(source), "-o", str(temp / "layout")], cwd=REPO, check=True)
    values = list(map(int, subprocess.check_output([str(temp / "layout")]).split()))
    return values[0], dict(zip(fields, values[1:]))


def read(remote, address, size):
    return b"".join(remote.memory(address + i, min(512, size - i)) for i in range(0, size, 512))


def run(mode, count):
    sym = symbols()
    log = REPO / "build" / f"smp-percpu-{mode}-{count}.log"
    report = {"firmware": mode, "cpus": count}
    with tempfile.TemporaryDirectory(prefix="fortress-percpu-") as temp:
        temp = Path(temp)
        stride, off = layout(temp)
        cmd = ["qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", "2G",
               "-smp", str(count), "-display", "none", "-S", "-no-reboot", "-monitor", "none",
               "-serial", f"file:{log}", "-gdb", f"unix:{temp}/gdb,server=on,wait=off",
               "-qmp", f"unix:{temp}/qmp,server=on,wait=off", "-boot", "d", "-cdrom", "bin/fortress.iso",
               "-drive", "file=build/nvme_gpt.img,if=none,id=nvm0,format=raw,snapshot=on",
               "-device", "nvme,serial=fortress0,drive=nvm0"]
        if mode == "uefi":
            shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", temp / "vars.fd")
            cmd += ["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
                    "-drive", f"if=pflash,format=raw,unit=1,file={temp}/vars.fd"]
        child = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        remote = qmp = None
        try:
            qmp, remote = QMP(temp / "qmp"), Remote(temp / "gdb")
            remote.request("qSupported")
            remote.request("qXfer:features:read:target.xml:0,fff")
            remote.resume_to(sym["smp_init"])
            # Entire BSP IST stacks, TSS and GDT must survive AP probes unchanged.
            regions = [(sym["ist1_memory"] + 4096, 16384),
                       (sym["ist2_memory"] + 4096, 16384),
                       (sym["cpu_tss"], 104), (sym["cpu_gdt"], 56)]
            before = [read(remote, addr, size) for addr, size in regions]
            remote.resume_to(sym["smp_percpu_ready"])
            assert before == [read(remote, addr, size) for addr, size in regions], "AP touched BSP IST/GDT/TSS"
            report["bsp_regions_unchanged"] = True
            cpus = []
            for i in range(count):
                base = sym["cpu_locals"] + i * stride
                data = read(remote, base, stride)
                cpu = {f: int.from_bytes(data[o:o + (4 if f in ("id", "lapic_id", "online", "probe") else 8)], "little")
                       for f, o in off.items()}
                assert cpu["self"] == base and cpu["id"] == i and cpu["online"] == 1, cpu
                assert cpu["probe"] == 0 and cpu["irq_depth"] == 0, cpu
                tss = read(remote, sym["cpu_tss"] + i * 104, 104)
                ist1, ist2 = struct.unpack_from("<2Q", tss, 36)
                assert ist1 == sym["ist1_memory"] + i * 20480 + 20480
                assert ist2 == sym["ist2_memory"] + i * 20480 + 20480
                if i:
                    assert cpu["current_thread"] == cpu["preempt_count"] == 0
                    assert ist1 - 16384 <= cpu["probe_rsp"] < ist1, "AP #DF missed local IST1"
                    assert cpu["rsp0"] == sym["ap_stacks"] + i * 20480 + 20480
                gdt = read(remote, sym["cpu_gdt"] + i * 56, 56)
                desc = gdt[40:56]
                tss_base = (int.from_bytes(desc[2:4], "little") | desc[4] << 16 |
                            desc[7] << 24 | int.from_bytes(desc[8:12], "little") << 32)
                assert tss_base == sym["cpu_tss"] + i * 104 and desc[5] == 0x8b
                cpus.append(cpu)
            assert len({cpu["lapic_id"] for cpu in cpus}) == count
            report["local_state_and_real_df"] = True
            # QMP x86 inject-nmi delivers to all CPUs, including halted APs.
            # Inspect CPU-local records after hardware delivery; no synthetic INT.
            # Finish boot banners before simultaneous UART reports can interleave.
            qmp.execute("cont")
            deadline = time.monotonic() + 45
            while time.monotonic() < deadline:
                if "fortress> " in log.read_text(errors="replace"):
                    break
                assert child.poll() is None, child.stderr.read().decode()
                time.sleep(0.1)
            else:
                raise AssertionError(f"Shell timeout: {log}")
            qmp.execute("stop")
            output = log.read_text(errors="replace")
            qmp.execute("inject-nmi")
            deadline = time.monotonic() + 5
            while True:
                qmp.execute("cont")
                time.sleep(0.05)
                qmp.execute("stop")
                delivered = [int.from_bytes(remote.memory(sym["cpu_locals"] + i * stride + off["nmi_count"], 8), "little")
                             for i in range(count)]
                if all(n == cpus[i]["nmi_count"] + 1 for i, n in enumerate(delivered)):
                    break
                assert time.monotonic() < deadline, delivered
            for i in range(count):
                base = sym["cpu_locals"] + i * stride
                nmis = int.from_bytes(remote.memory(base + off["nmi_count"], 8), "little")
                rsp = int.from_bytes(remote.memory(base + off["nmi_rsp"], 8), "little")
                assert nmis == cpus[i]["nmi_count"] + 1, (i, nmis)
                bottom = sym["ist2_memory"] + i * 20480 + 4096
                assert bottom <= rsp < bottom + 16384, (i, hex(rsp))
            checkpoint = output.split("SMP Piece 1: AP Discovery", 1)[1]
            assert "[FAIL]" not in checkpoint and "[WARN]" not in checkpoint, checkpoint
            assert "SMP Piece 2 per-CPU storage ready" in checkpoint
            assert checkpoint.count("GS/TSS/IST1 fault/IST2 probe passed") == count - 1
            report["hardware_nmi_each_cpu"] = True
            report["shell_reached"] = True
            (REPO / "build" / f"smp-percpu-{mode}-{count}.json").write_text(json.dumps(report, indent=2) + "\n")
            print(f"PASS {mode} -smp {count}: distinct GS/GDT/TSS/stacks, BSP isolation, AP #DF, hardware NMI, shell", flush=True)
        finally:
            if remote: remote.sock.close()
            if qmp:
                qmp.stream.close()
                qmp.sock.close()
            child.terminate()
            try: child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
            if child.stderr: child.stderr.close()


if __name__ == "__main__":
    for firmware in ("bios", "uefi"):
        for cpus in (1, 4, 8):
            run(firmware, cpus)
