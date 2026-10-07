"""Calibrate QEMU blkdebug error arming on an owned regular-file backend."""
import json
from pathlib import Path
import subprocess
import tempfile
from test_nmi_transitions import REPO,QMP


def main():
    out=Path(tempfile.mkdtemp(prefix='usb-backend-probe-',dir=REPO/'.codex-remote-attachments/ext4-phase9'))
    sockets=Path(tempfile.mkdtemp(prefix='fortress-usb-probe-'));disk=out/'probe.img';disk.write_bytes(bytes(4096));results=[]
    cmd=['qemu-system-x86_64','-machine','none','-nodefaults','-display','none','-S',
         '-qmp',f'unix:{sockets/"qmp"},server=on,wait=off',
         '-blockdev',json.dumps({'driver':'file','filename':str(disk),'node-name':'file'}),
         '-blockdev',json.dumps({'driver':'blkdebug','image':'file','node-name':'debug',
             'inject-error':[{'event':'write_aio','state':2,'errno':5,'once':True,'immediately':True}],
             'set-state':[{'event':'pwritev_zero','state':1,'new_state':2}]}),
         '-blockdev',json.dumps({'driver':'raw','file':'debug','node-name':'e4'})]
    with (out/'stderr.log').open('wb') as error:
        process=subprocess.Popen(cmd,stderr=error,stdout=subprocess.DEVNULL);qmp=None
        try:
            qmp=QMP(sockets/'qmp')
            schema=qmp.execute('query-qmp-schema');(out/'schema.json').write_text(json.dumps(schema,indent=2))
            for command in ('help qemu-io','qemu-io e4 "write 0 512"'):
                results.append({'command':command,'reply':qmp.execute('human-monitor-command',{'command-line':command})})
            try:
                reply=qmp.execute('blockdev-reopen',{'options':[{'driver':'blkdebug','node-name':'e4','image':'raw',
                    'inject-error':[{'event':'write_aio','errno':5,'once':True,'immediately':True}]}]})
                results.append({'reopen':reply})
                for command in ('qemu-io e4 "write 512 512"','qemu-io e4 "write 1024 512"'):
                    results.append({'command':command,'reply':qmp.execute('human-monitor-command',{'command-line':command})})
            except AssertionError as exception:results.append({'reopen_error':str(exception)})
            try:
                qmp.execute('blockdev-add',{'driver':'blkdebug','node-name':'fault','image':'raw',
                    'inject-error':[{'event':'write_aio','errno':5,'once':True,'immediately':True}]})
                reply=qmp.execute('blockdev-reopen',{'options':[{'driver':'blkdebug','node-name':'e4','image':'fault'}]})
                results.append({'replace_child':reply})
                for command in ('qemu-io e4 "write 512 512"','qemu-io e4 "write 1024 512"'):
                    results.append({'command':command,'reply':qmp.execute('human-monitor-command',{'command-line':command})})
            except AssertionError as exception:results.append({'replace_error':str(exception)})
            for command in ('qemu-io e4 "write -z 2048 512"','qemu-io e4 "write -P 90 512 512"','qemu-io e4 "write -P 91 1024 512"'):
                results.append({'arm_command':command,'reply':qmp.execute('human-monitor-command',{'command-line':command})})
            data=disk.read_bytes();results.append({'failed_region_unchanged':data[512:1024]==bytes(512),'retry_region_written':data[1024:1536]==bytes([91])*512})
        finally:
            if qmp:qmp.sock.close()
            process.kill();process.wait(timeout=10)
            (out/'manifest.json').write_text(json.dumps({'argv':cmd,'results':results},indent=2)+'\n')
            (sockets/'qmp').unlink(missing_ok=True);sockets.rmdir()
    print(json.dumps(results,indent=2));print(out)


if __name__=='__main__':main()
