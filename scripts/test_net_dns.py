"""Persistent real Ring 3 DNS matrix; port-53 fixture, no data disks."""
import argparse
import concurrent.futures
import json
import socket
import time
import traceback
import uuid
from test_net_tcp_matrix import Case, REPO, save
from net_dns_peer import DNSPeer, HostDNS
from net_dns_wire_audit import audit

def run(root,mode,model,backend,cpus=1,shared_host=None):
    case=Case(root,mode,model,backend,cpus); host=None; rows=[]
    tag=f'{mode}-{model}-{backend}-s{cpus}'
    case.config=dict(profiles=['dns-A','CNAME','NXDOMAIN','malformed','timeout','TC-TCP','stall','trickle','EOF','hostname-nc'])
    try:
        if backend=='user': host=shared_host or HostDNS()
        case.boot()
        if case.peer: case.peer=DNSPeer(case.peer)
        def execute(command,expected,name=None,timeout=45):
            if name:
                command=command.replace(name,name.replace('.test',f'.{tag}.test'))
                expected=expected.replace('service.test',f'service.{tag}.test')
                name=name.replace('.test',f'.{tag}.test')
            at=case.start(command); started=time.monotonic(); output=case.finish(at,timeout)
            assert expected in output,output
            rows.append(dict(command=command,expected=expected,dns_name=name,output=output,elapsed=time.monotonic()-started))
            save(root/'dns-cases.json',rows); return output
        # No need to warm up for UDP; explicit quiet-time failure is tested.
        execute('nslookup -s 10.0.2.2 service.test','Address: 10.0.2.2','service.test')
        execute('nslookup -s 10.0.2.2 alias.test','Name: service.test','alias.test')
        execute('nslookup -s 10.0.2.2 missing.test','nslookup: DNS_NXDOMAIN','missing.test')
        execute('nslookup -s 10.0.2.2 bad.test','nslookup: DNS_MALFORMED','bad.test')
        execute('nslookup -s 10.0.2.2 192.0.2.7','Address: 192.0.2.7')
        execute('nslookup -s 10.0.2.2 timeout.test','nslookup: DNS_TIMEOUT','timeout.test')
        # Use the existing actual numeric client gate to wait through quiet time,
        # proving fallback isn't silently implemented by a polling resolver.
        case.client(7777)
        execute('nslookup -s 10.0.2.2 tcp.test','Address: 10.0.2.2','tcp.test')
        execute('nslookup -s 10.0.2.2 stall.test','nslookup: DNS_TIMEOUT','stall.test')
        execute('nslookup -s 10.0.2.2 trickle.test','nslookup: DNS_TIMEOUT','trickle.test')
        execute('nslookup -s 10.0.2.2 eof.test','nslookup: DNS_IO','eof.test')
        listener=pool=future=None
        try:
            if case.peer: case.peer.listen(7784,b'hello\n',b'dns-nc-response\n'); port=7784
            else:
                listener=socket.socket(); listener.bind(('127.0.0.1',0)); listener.listen(1); listener.settimeout(20); port=listener.getsockname()[1]
                pool=concurrent.futures.ThreadPoolExecutor(1)
                def echo():
                    with listener.accept()[0] as s:
                        s.settimeout(20); data=b''
                        while True:
                            part=s.recv(4096)
                            if not part: break
                            data+=part; assert len(data)<=6
                        assert data==b'hello\n'; s.sendall(b'dns-nc-response\n'); s.shutdown(socket.SHUT_WR)
                future=pool.submit(echo)
            execute(f'echo hello | nc -s 10.0.2.2 service.test {port}','\ndns-nc-response\n','service.test')
            if future:
                future.result(timeout=1)
                case.expected.append(dict(port=port,guest_port=None,expected=b'hello\n'.hex(),response=b'dns-nc-response\n'.hex(),active=False))
        finally:
            if listener: listener.close()
            if pool: pool.shutdown(wait=False,cancel_futures=True)
        execute('ping -c 1 10.0.2.2','1 probes, 1 replies')
        execute('echo dns-shell-recovered','\ndns-shell-recovered\n')
        assert 'PANIC' not in case.text() and '[FATAL]' not in case.text()
    except BaseException:
        case.failure=traceback.format_exc(); case.diagnostics()
    finally:
        save(root/'dns-cases.json',rows)
        if host: case.expected.extend(dict(s) for s in host.streams if s['dns_name'].endswith(f'.{tag}.test'))
        for name in ('net_dns_peer.py','net_dns_wire_audit.py','test_net_dns.py'):
            (root/name).write_bytes((REPO/'scripts'/name).read_bytes())
        try: case.close()
        finally:
            if host and not shared_host: host.close()
    try:
        result=audit(root); save(root/'dns-audit.json',result)
    except BaseException:
        manifest=json.loads((root/'result.json').read_text()); manifest['status']='FAIL'; manifest['dns_audit_error']=traceback.format_exc()
        save(root/'result.json',manifest); raise
    print('[PASS] DNS UDP/TCP real Ring 3 + independent wire audit:',root,flush=True)

def main():
    parser=argparse.ArgumentParser(); parser.add_argument('--all',action='store_true')
    parser.add_argument('--jobs',type=int,choices=(1,2,3,4),default=1)
    parser.add_argument('--case',default='bios-e1000-socket',choices=[f'{m}-{n}-{b}' for m in ('bios','uefi') for n in ('e1000','e1000e') for b in ('user','socket')])
    args=parser.parse_args(); root=REPO/'build/net2-step7'/uuid.uuid4().hex[:8]
    print('DNS matrix artifacts:',root,flush=True)
    labels=[args.case] if not args.all else [f'{m}-{n}-{b}' for m in ('bios','uefi') for n in ('e1000','e1000e') for b in ('user','socket')]
    cases=[(label,1) for label in labels]
    if args.all: cases.extend((f'{m}-e1000-user',4) for m in ('bios','uefi'))
    host=HostDNS() if any(label.endswith('-user') for label,_ in cases) else None
    try:
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as executor:
            futures=[executor.submit(run,root/f'{label}-s{cpus}',*label.split('-'),cpus=cpus,shared_host=host) for label,cpus in cases]
            for future in futures: future.result()
    finally:
        if host: host.close()
    print(f'DNS matrix {len(cases)}/{len(cases)} PASS; physical acceptance remains manual',flush=True)

if __name__=='__main__': main()
