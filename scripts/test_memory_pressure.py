"""Real PMM exhaustion versus retained heap backing; ISO-only QEMU diagnostics."""
import datetime
import json
from pathlib import Path
import re
import shutil
import subprocess
import time
from test_smp_memory_boot import make_iso, CODE, VARS, REPO
from test_memory_ext4 import digest


def main():
    out=REPO/'build'/('memory-pressure-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ'))
    out.mkdir(parents=True)
    tracked=[REPO/'bin/fortress.img',REPO/'bin/fortress.elf',REPO/'docs/plans/PERMISSIONS_PLAN.md']
    before={str(p):digest(p) for p in tracked}
    record={'status':'FAIL','before':before,'cases':[]}
    try:
        with (out/'fixture.log').open('w') as log:
            # make_iso retains the existing explicit 6A boot checks too.
            import contextlib
            with contextlib.redirect_stdout(log):
                iso=make_iso(out)
        for firmware,ram,enabled in [('bios','256M',True),('uefi','256M',True),
                                     ('bios','512M',True),('uefi','512M',True),('bios','256M',False)]:
            label=f'{firmware}-{ram}-'+('pressure' if enabled else 'control')
            uart=out/(label+'.log');stderr=out/(label+'.stderr')
            command=['qemu-system-x86_64','-accel','tcg','-M','q35','-m',ram,'-smp','1',
                     '-display','none','-monitor','none','-no-reboot','-serial',f'file:{uart}',
                     '-boot','d','-cdrom',str(iso)]
            drives=[]
            if firmware=='uefi':
                assert CODE.is_file() and VARS.is_file()
                variables=out/(label+'-vars.fd');shutil.copyfile(VARS,variables)
                drives=[f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
                        f'if=pflash,format=raw,unit=1,file={variables}']
                for drive in drives:command+=['-drive',drive]
            if enabled:command+=['-fw_cfg','name=opt/fortress/memory_pressure_test,string=1']
            assert [command[i+1] for i,a in enumerate(command) if a=='-drive']==drives
            assert not any(a in command for a in ('-device','-blockdev','-hda','-hdb'))
            case={'firmware':firmware,'ram':ram,'enabled':enabled,'argv':command,'status':'FAIL'}
            record['cases'].append(case)
            (out/(label+'.argv.json')).write_text(json.dumps(command,indent=2)+'\n')
            start=time.monotonic()
            with stderr.open('w') as err:
                child=subprocess.Popen(command,cwd=REPO,stdout=subprocess.DEVNULL,stderr=err)
                try:
                    while time.monotonic()-start<180:
                        text=uart.read_text(errors='replace') if uart.exists() else ''
                        assert '[FAIL]' not in text, f'boot assertion: {uart}'
                        assert 'KERNEL PANIC' not in text, f'panic: {uart}'
                        if re.search(r'fortress:[^\r\n]* \$ ',text):break
                        assert child.poll() is None, f'QEMU exited early: {stderr}'
                        time.sleep(.1)
                    else:raise TimeoutError(f'shell timeout: {uart}')
                finally:
                    if child.poll() is None:child.terminate()
                    try:child.wait(timeout=5)
                    except subprocess.TimeoutExpired:child.kill();child.wait(timeout=5)
            text=uart.read_text(errors='replace')
            if enabled:
                pattern=r'\[MEMORY PRESSURE\] (initial|retained) held_pages=(\d+) heap_free=(\d+) heap_committed=(\d+) largest_payload=(\d+) oom_delta=(\d+) tables=(\d+) PASS exact bitmap/heap cleanup'
                rows=re.findall(pattern,text);assert [r[0] for r in rows]==['initial','retained']
                case['measurements']={r[0]:dict(zip(('held_pages','heap_free','heap_committed','largest_payload','oom_delta','tables'),map(int,r[1:]))) for r in rows}
                match=re.search(r'\[MEMORY PRESSURE\] PASS retained_data_pages=(\d+) retained_table_pages=(\d+) page_consumer_capacity_lost=(\d+)',text)
                assert match
                data,tables,lost=map(int,match.groups());assert data+tables==lost
                initial=case['measurements']['initial'];retained=case['measurements']['retained']
                assert initial['held_pages']-retained['held_pages']==lost
                assert retained['heap_free']>=4*1024*1024 and all(r['oom_delta']>=3 for r in case['measurements'].values())
                case['retained_data_pages']=data;case['retained_table_pages']=tables;case['capacity_lost']=lost
            else:assert '[MEMORY PRESSURE]' not in text
            case['status']='PASS';case['elapsed_seconds']=round(time.monotonic()-start,2)
            print(f'PASS memory pressure {label}: {uart}',flush=True)
        record['status']='PASS'
    finally:
        record['after']={str(p):digest(p) for p in tracked}
        if record['after']!=before:record['status']='FAIL'
        (out/'result.json').write_text(json.dumps(record,indent=2)+'\n')
        print(f"Memory pressure {record['status']}: {out}",flush=True)
        assert record['after']==before


if __name__=='__main__':main()
