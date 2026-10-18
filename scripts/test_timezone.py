#!/usr/bin/env python3
"""Disk-free BIOS/UEFI Ring 3 time acceptance against a local UDP NTP oracle.

Uses a disposable ISO and existing base archive; release images stay untouched.
No Internet NTP service, physical target or retained account secret is used.
"""
from pathlib import Path
import hashlib
import io
import re
import shutil
import socket
import struct
import subprocess
import tarfile
import tempfile
import threading
import time
import test_logging
import build_zoneinfo

ROOT=Path(__file__).resolve().parent.parent

def run(args):
    subprocess.run([str(a) for a in args],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)

def fixture(tmp):
    tree=tmp/'tree';(tree/'boot/limine').mkdir(parents=True);(tree/'EFI/BOOT').mkdir(parents=True)
    for name in ('limine-bios.sys','limine-bios-cd.bin','limine-uefi-cd.bin'):
        shutil.copyfile(ROOT/'limine'/name,tree/'boot/limine'/name)
    shutil.copyfile(ROOT/'limine/BOOTX64.EFI',tree/'EFI/BOOT/BOOTX64.EFI')
    config='timeout: 0\n/Time acceptance\n    protocol: limine\n    kernel_path: boot():/boot/fortress.elf\n    module_path: boot():/boot/initramfs.tar\n    kernel_cmdline: net=dhcp\n'
    for name in ('boot/limine/limine.conf','boot/limine.conf'):(tree/name).write_text(config)
    shutil.copyfile(ROOT/'bin/fortress.elf',tree/'boot/fortress.elf')
    files={
        'bin/date':('build/tool-date.elf',0o755),'bin/logger':('build/tool-logger.elf',0o755),
        'sbin/ntpd':('build/ntpd-test.elf',0o755),'sbin/dhcpc':('build/dhcpc.elf',0o755),
        'bin/time-clock-probe':('build/time-clock-probe.elf',0o755),
    }
    replacements={name:((ROOT/path).read_bytes(),mode) for name,(path,mode) in files.items()}
    replacements['etc/watchtower/ntpd.service']=(b'Exec=/sbin/ntpd -f 10.0.2.2\nOrder=15\nEnabled=no\nRestart=on-failure\nTimeoutStop=2\n',0o644)
    replacements['usr/share/zoneinfo.tzdb']=((ROOT/'build/zoneinfo.tzdb').read_bytes(),0o644)
    # Readiness and ssh keys are unrelated to this disk-free fixture.
    replacements['etc/watchtower/sshd.service']=(b'Exec=/sbin/sshd -D\nEnabled=no\n',0o644)
    archive=tree/'boot/initramfs.tar'
    with tarfile.open(ROOT/'bin/initramfs.tar','r:') as src,tarfile.open(archive,'w',format=tarfile.USTAR_FORMAT) as dst:
        for entry in src:
            name=entry.name.removeprefix('./')
            if name in replacements or name.startswith('usr/share/zoneinfo/'):continue
            dst.addfile(entry,src.extractfile(entry) if entry.isfile() else None)
        for name,(data,mode) in replacements.items():
            entry=tarfile.TarInfo(name);entry.size=len(data);entry.mode=mode;dst.addfile(entry,io.BytesIO(data))
    assert archive.stat().st_size<=8*1024*1024,'installer bootstrap size contract'
    shutil.copyfile(archive,tree/'initramfs.tar');(tmp/'bin').mkdir();iso=tmp/'bin/fortress.iso'
    run(['xorriso','-as','mkisofs','-b','boot/limine/limine-bios-cd.bin','-no-emul-boot',
         '-boot-load-size','4','-boot-info-table','--efi-boot','boot/limine/limine-uefi-cd.bin',
         '-efi-boot-part','--efi-boot-image','--protective-msdos-label',tree,'-o',iso])
    run([ROOT/'limine/limine','bios-install',iso])

class Oracle:
    def __init__(self):
        self.mode='valid';self.stop=False;self.requests=0
        self.socket=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        self.socket.bind(('127.0.0.1',43123));self.socket.settimeout(.1)
        self.thread=threading.Thread(target=self.work,daemon=True);self.thread.start()
    def work(self):
        while not self.stop:
            try:data,peer=self.socket.recvfrom(2048)
            except socket.timeout:continue
            if len(data)!=48:continue
            self.requests+=1
            if self.mode=='silent':continue
            packet=bytearray(48);packet[0]=0x24;packet[1]=2;packet[24:32]=data[40:48]
            # 2026-07-01 00:00:00 UTC, exact sample oracle.
            stamp=struct.pack('!II',1782864000+2208988800,0)
            packet[32:40]=stamp;packet[40:48]=stamp
            if self.mode=='nonce':packet[24]^=1
            if self.mode=='kod':packet[1]=0
            self.socket.sendto(packet,peer)
    def close(self):
        self.stop=True;self.thread.join(timeout=1);self.socket.close()

def test(firmware,smp,tmp,oracle):
    guest=test_logging.Guest(firmware,smp,tmp)
    try:
        guest.wait(lambda t:t.endswith('FortressOS login: '));guest.send('operator');guest.wait(lambda t:t.endswith('$ '))
        session_start=len(guest.data)
        def cmd(text):
            result=guest.command(text);assert '[PROCESS FAULT]' not in result,result;return result
        assert 'CLOCK_PROBE_PASS' in cmd('sudo /bin/time-clock-probe')
        assert 'CAP_SYS_ADMIN required' in cmd('/sbin/ntpd --once 10.0.2.2')
        oracle.mode='valid'
        assert 'UTC synchronized' in cmd('sudo /sbin/ntpd --once 10.0.2.2')
        assert '2026-07-01T00:00:' in cmd('date -u -I')
        assert '2026-07-01T02:00:' in cmd('TZ=Europe/Brussels date -I')
        assert '2026-06-30T20:00:' in cmd('TZ=America/New_York date -I')
        assert '2026-07-01T05:45:' in cmd('TZ=Asia/Kathmandu date -I')
        assert 'Z' in cmd('TZ=Europe/Brussels date -u -I')
        assert 'invalid or unavailable timezone' in cmd('TZ=../../etc/shadow date')
        for mode in ('nonce','kod','silent'):
            oracle.mode=mode
            begin=time.monotonic();result=cmd('sudo /sbin/ntpd --once 10.0.2.2')
            assert 'synchronization failed; clock unchanged' in result,result
            assert time.monotonic()-begin<9
            assert '2026-07-01T00:00:' in cmd('date -u -I')
        oracle.mode='valid'
        assert 'UTC synchronized' in cmd('sudo /sbin/ntpd --once 10.0.2.2')
        assert 'State: stopped' in cmd('sudo service status ntpd')
        cmd('sudo service start ntpd')
        for _ in range(20):
            if 'UTC synchronized' in cmd('sudo cat /run/watchtower/ntpd.log'):break
            cmd('sleep 0.1')
        else:raise AssertionError('supervised NTP did not synchronize')
        begin=time.monotonic();cmd('sudo service stop ntpd');assert time.monotonic()-begin<5
        assert 'State: stopped' in cmd('sudo service status ntpd')
        assert 'uid=0' in cmd('sudo id')
        assert '[PROCESS FAULT]' not in guest.text(session_start)
        print(f'PASS {firmware} SMP={smp}: clock boundaries, monotonic isolation, real UDP SNTP, timezone/DST, rejection deadlines, supervised start/stop',flush=True)
    finally:
        (ROOT/'build'/f'timezone-{firmware}-{smp}.log').write_text(guest.text())
        guest.close()

def main():
    retained=[ROOT/'bin'/name for name in ('fortress.iso','fortress.img','initramfs.tar')]
    before={p:hashlib.sha256(p.read_bytes()).digest() for p in retained if p.exists()}
    oracle=Oracle()
    try:
        with tempfile.TemporaryDirectory(prefix='fortress-time-') as directory:
            tmp=Path(directory);fixture(tmp);test_logging.ROOT=tmp
            for firmware in ('bios','uefi'):
                for smp in (1,4):test(firmware,smp,tmp,oracle)
    finally:oracle.close()
    assert all(hashlib.sha256(p.read_bytes()).digest()==digest for p,digest in before.items())
    print('PASS release ISO/image/archive unchanged; clock probe excluded from release packaging')

if __name__=='__main__':main()
