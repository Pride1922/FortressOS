#!/usr/bin/env python3
"""Finite trace, synthetic routers and independent audit. Disposable ISO/OVMF,
no data disks; retained failures and bounded independent UART drain.
"""
import argparse
import concurrent.futures
import hashlib
import io
import json
from pathlib import Path
import shutil
import socket
import subprocess
import tarfile
import time
import traceback
import uuid
from test_net_tcp_matrix import Case, save
from test_net_pci import REPO, CODE, VARS
from net_trace_peer import Peer
from net_trace_wire_audit import audit, negative_gates


def build_iso(root):
    source=root/"iso-root"
    shutil.copytree(REPO/"build/iso_root",source)
    payload=(REPO/"build/trace_probe.elf").read_bytes()
    with tarfile.open(source/"boot/initramfs.tar","a",format=tarfile.USTAR_FORMAT) as archive:
        info=tarfile.TarInfo("bin/trace-probe"); info.size=len(payload); info.mode=0o755
        archive.addfile(info,io.BytesIO(payload))
    config=("timeout: 0\n/FortressOS trace test\n    protocol: limine\n"
            "    kernel_path: boot():/boot/fortress.elf\n"
            "    module_path: boot():/boot/initramfs.tar\n    kernel_cmdline: net_test=arp\n")
    for path in (source/"limine.conf",source/"boot/limine.conf",source/"boot/limine/limine.conf"):
        path.write_text(config)
    iso=root/"trace.iso"
    subprocess.run(["xorriso","-as","mkisofs","-b","boot/limine/limine-bios-cd.bin",
                    "-no-emul-boot","-boot-load-size","4","-boot-info-table",
                    "--efi-boot","boot/limine/limine-uefi-cd.bin","-efi-boot-part",
                    "--efi-boot-image","--protective-msdos-label",str(source),"-o",str(iso)],
                   check=True,timeout=60,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    subprocess.run([str(REPO/"limine/limine"),"bios-install",str(iso)],check=True,timeout=30,
                   stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    return iso


class TraceCase(Case):
    def boot(self):
        iso=build_iso(self.root)
        self.uart_path=Path("/tmp")/("fortress-trace-"+uuid.uuid4().hex[:12])
        self.host=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        self.host.bind(("127.0.0.1",0))
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as reservation:
            reservation.bind(("127.0.0.1",0)); guest_port=reservation.getsockname()[1]
        injections=(self.root/"injection.jsonl").open("w"); self.files.append(injections)
        self.peer=Peer(self.host,("127.0.0.1",guest_port),injections)
        cmd=["qemu-system-x86_64","-M","q35","-m","2G","-accel","tcg","-smp","1",
             "-display","none","-monitor","none","-no-reboot","-boot","d","-cdrom",str(iso),
             "-chardev",f"socket,id=uart,path={self.uart_path},server=on,wait=on","-serial","chardev:uart",
             "-netdev",f"socket,id=net0,udp=127.0.0.1:{self.host.getsockname()[1]},localaddr=127.0.0.1:{guest_port}",
             "-device",f"{self.model},netdev=net0,mac=52:54:00:12:34:56",
             "-object",f"filter-dump,id=dump0,netdev=net0,file={self.pcap}"]
        if self.mode=="uefi":
            variables=self.root/"vars.fd"; shutil.copyfile(VARS,variables)
            cmd+=["-drive",f"if=pflash,format=raw,unit=0,readonly=on,file={CODE}",
                  "-drive",f"if=pflash,format=raw,unit=1,file={variables}"]
        frozen=tuple(cmd)
        def preflight(candidate): assert tuple(candidate)==frozen,"unauthorized argv/storage"
        preflight(cmd)
        for extra in (["-drive","file=unsafe.img"],["-device","nvme"],["-hda","unsafe.img"]):
            try: preflight(cmd+extra)
            except AssertionError: pass
            else: raise AssertionError("unsafe argv accepted")
        save(self.root/"argv.json",cmd)
        versions=subprocess.check_output(["qemu-system-x86_64","--version"],text=True)
        versions+="git "+subprocess.check_output(["git","rev-parse","HEAD"],cwd=REPO,text=True)
        for name in ("test_net_trace.py","net_trace_peer.py","net_trace_wire_audit.py"):
            raw=(REPO/"scripts"/name).read_bytes(); (self.root/name).write_bytes(raw)
            versions+=name+" sha256 "+hashlib.sha256(raw).hexdigest()+"\n"
        (self.root/"versions.txt").write_text(versions)
        stderr=(self.root/"qemu-stderr.log").open("wb"); self.files.append(stderr)
        self.proc=subprocess.Popen(cmd,cwd=REPO,stdout=subprocess.DEVNULL,stderr=stderr)
        end=time.monotonic()+20
        while time.monotonic()<end:
            candidate=socket.socket(socket.AF_UNIX)
            try: candidate.connect(str(self.uart_path)); self.uart=candidate; break
            except OSError: candidate.close(); time.sleep(.03)
        assert self.uart is not None,"UART creation timeout"
        self.uart.settimeout(.05); self.start_reader()
        self.wait(lambda text: "fortress> " in text and "Gateway ARP resolved" in text,60)

    def command(self,value,timeout=20):
        return self.finish(self.start(value),timeout)

    def status(self,expected):
        out=self.command("echo $?")
        assert str(expected) in out.splitlines(),out

    def scenario(self,profile,command,ttls,status):
        self.peer.profile=profile
        at=len(self.peer.traces)
        out=self.command(command)
        self.status(status)
        assert [row["ttl"] for row in self.peer.traces[at:]]==ttls,out
        self.expected.append(dict(profile=profile,command=command,ttls=ttls,status=status))
        save(self.root/"scenarios.json",self.expected)
        return out

    def close(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try: self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired: self.proc.kill(); self.proc.wait(timeout=3)
        self.reader_stop.set()
        if self.uart:
            try: self.uart.shutdown(socket.SHUT_RDWR)
            except OSError: pass
            self.uart.close()
        if self.reader: self.reader.join(timeout=2)
        self.flush_serial()
        if self.host: self.host.close()
        if hasattr(self,"uart_path"): self.uart_path.unlink(missing_ok=True)
        for stream in self.files: stream.flush(); stream.close()
        save(self.root/"scenarios.json",self.expected)
        try:
            self.audit_result=audit(self.pcap,self.root/"injection.jsonl",self.expected)
            negative_gates(self.root)
        except Exception:
            self.failure=(self.failure or "")+"\nIndependent audit: "+traceback.format_exc()
        if self.reader_error: self.failure=(self.failure or "")+self.reader_error
        save(self.root/"result.json",dict(status="FAIL" if self.failure else "PASS",error=self.failure,
             audit=self.audit_result,pid=None if self.proc is None else self.proc.pid,
             reaped=self.proc is None or self.proc.poll() is not None))
        if self.failure: raise AssertionError(self.failure)


def run(root,mode,model):
    case=TraceCase(root,mode,model,"socket")
    try:
        case.boot()
        out=case.scenario("clean","traceroute -m 4 192.0.2.9",[1,1,1,2,2,2,3],0)
        lines=out.splitlines()
        assert sum(line.startswith("1  10.0.2.2") for line in lines)==3,out
        assert sum(line.startswith("2  10.0.2.3") for line in lines)==3,out
        assert any(line.startswith("3  192.0.2.9") for line in lines),out
        out=case.scenario("forged","traceroute -m 4 -q 1 192.0.2.9",[1,2,3],0)
        assert "192.0.2.66" not in out,out
        out=case.scenario("unreachable","traceroute -m 4 -q 1 192.0.2.9",[1],1)
        assert " !H\n" in out,out
        out=case.scenario("drop","traceroute -m 1 -q 1 192.0.2.9",[1],1)
        assert "\n1  *\n" in out,out
        # Real Ring 3 writable-range/reserved and sub-probe command deadline.
        out=case.scenario("drop","/bin/trace-probe",[1],0)
        assert "TRACE ABI deadline/range/reserved PASS" in out,out
        # Existing ping owns admission: trace exits immediately without a retry.
        case.peer.profile="drop"
        ping_count=case.peer.pings+1
        at=case.start("ping -c 1 -W 5 192.0.2.9 &")
        # Background output may follow the prompt. Waiting for a trailing prompt
        # would sometimes wait until ping exits, defeating the contention test.
        case.wait(lambda text: "fortress> " in text[at:] and case.peer.pings>=ping_count,10)
        out=case.scenario("drop","traceroute -m 1 -q 1 192.0.2.9",[],1)
        assert "ping/trace busy; retry manually" in out,out
        case.wait(lambda text: "1 probes, 0 replies" in text,15)
        # Default SIGINT exit must leave only a finite lease (caught path is host-tested).
        case.peer.profile="drop"
        wanted=len(case.peer.traces)+1
        at=case.start("traceroute -m 1 -q 1 -W 5 192.0.2.9")
        case.wait(lambda text: len(case.peer.traces)>=wanted,10)
        case.expected.append(dict(profile="interrupted",command="Ctrl-C trace",ttls=[1],status=130))
        case.uart.sendall(b"\x03")
        case.finish(at,10)
        case.status(130)
        until=time.monotonic()+8
        while time.monotonic()<until: case.pump()
        out=case.scenario("clean","traceroute -m 4 -q 1 192.0.2.9",[1,2,3],0)
        assert "192.0.2.9" in out,out
        out=case.command("ping -c 1 10.0.2.2")
        assert "1 probes, 1 replies" in out,out
        case.status(0)
        assert "PANIC" not in case.text() and "[FATAL]" not in case.text()
    except BaseException:
        case.failure=traceback.format_exc()
    finally:
        case.close()
    print(f"PASS {mode}-{model}: trace/router/deadline/ABI/contention/cancel + independent wire audit",flush=True)


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--jobs",type=int,choices=(1,2,3,4),default=2)
    args=parser.parse_args()
    assert CODE.exists() and VARS.exists()
    root=REPO/"build/net-trace"/uuid.uuid4().hex[:8]
    cases=[(mode,model) for mode in ("bios","uefi") for model in ("e1000","e1000e")]
    print("Trace artifacts:",root,flush=True)
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.jobs) as pool:
        futures=[pool.submit(run,root/f"{mode}-{model}",mode,model) for mode,model in cases]
        for future in concurrent.futures.as_completed(futures): future.result()
    print("Trace matrix 4/4 PASS; synthetic topology, no physical claim.")


if __name__=="__main__": main()
