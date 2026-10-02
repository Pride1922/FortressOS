"""Real Ring 3 caught signal, STOP/CONT and KILL on timed TCP fallback."""
import re
import time
import traceback
import uuid
from test_net_tcp_matrix import Case,REPO,save
from net_dns_peer import DNSPeer

def run(root,mode):
    case=Case(root,mode,'e1000','socket'); case.config=dict(profiles=['timed-caught','timed-stop-cont','timed-kill'])
    try:
        case.boot(); case.peer=DNSPeer(case.peer); case.client(7777)
        at=case.start('dnsprobe 10.0.2.2 stall.test --caught')
        case.wait(lambda _: any(c.port==53 and len(c.consumed)==len(c.expected) for c in case.peer.connections),5)
        case.uart.sendall(b'\x03')
        assert 'DNS probe DNS_INTERRUPTED' in case.finish(at,10)
        at=case.start('dnsprobe 10.0.2.2 stall.test &'); output=case.finish(at)
        job=re.search(r'\[(\d+)\]\s+(\d+)',output); assert job,output
        case.wait(lambda _: sum(c.port==53 and len(c.consumed)==len(c.expected) for c in case.peer.connections)>=2,5)
        case.finish(case.start(f'kill %{job[1]} STOP'))
        until=time.monotonic()+6
        while time.monotonic()<until: case.pump()
        resumed=case.start(f'kill %{job[1]} CONT'); case.finish(resumed)
        case.wait(lambda t: 'DNS probe DNS_TIMEOUT' in t[at:],10)
        output=case.finish(case.start('dnsprobe 10.0.2.2 stall.test &'))
        job=re.search(r'\[(\d+)\]\s+(\d+)',output); assert job,output
        case.wait(lambda _: sum(c.port==53 and len(c.consumed)==len(c.expected) for c in case.peer.connections)>=3,5)
        case.finish(case.start(f'kill %{job[1]} KILL'))
        case.wait(lambda _: all(c.done for c in case.peer.connections),10)
        assert '\ndeadline-shell-recovered\n' in case.finish(case.start('echo deadline-shell-recovered'))
        assert 'PANIC' not in case.text() and '[FATAL]' not in case.text()
        assert '1 probes, 1 replies' in case.finish(case.start('ping -c 1 10.0.2.2'))
    except BaseException:
        case.failure=traceback.format_exc(); case.diagnostics()
    finally:
        for name in ('test_net_dns_lifecycle.py','net_dns_peer.py'):
            (root/name).write_bytes((REPO/'scripts'/name).read_bytes())
        case.close()
    print('[PASS] real timed fallback caught SIGINT / STOP beyond deadline / CONT / KILL / close / shell:',root,flush=True)

if __name__=='__main__':
    root=REPO/'build/net2-step7'/uuid.uuid4().hex[:8]; print('Deadline lifecycle artifacts:',root,flush=True)
    for mode in ('bios','uefi'): run(root/mode,mode)
