"""Real Ring 3 blocked-SEND/CONNECT deadlines with independent bounded audit."""
import json
import time
import traceback
import uuid
import test_net_tcp_matrix as matrix
from net_tcp_socket_peer import Connection,frame
from net_tcp_wire_audit import records,parse
from net_dns_peer import DNSPeer

class ZeroWindow(Connection):
    def wire(self,flags,data=b'',at=None,ack=None):
        return frame(self.port,self.guest_port,self.iss+(self.next if at is None else at),
                     0 if self.irs is None else self.irs+(self.rx if ack is None else ack),
                     flags,data,window=0,mss=self.mss if flags&2 else None)
    def input(self,packet):
        if packet['data']:
            assert len(packet['data'])==1 and packet['data']==b'Z'
            self.peer.event('zero-window-unacked-probe',port=self.port); return
        if packet['flags']&4: self.done=True; self.state='DONE'; return
        super().input(packet)

def deadline_audit(pcap,injection_path,scenarios):
    """No peer helper imports: wire controls/window/bytes prove finite boundary.
    Accepted TX is deliberately not fully delivered, so this isn't a clean-FIN
    full-transfer case. Warmup stream remains exact and capture inputs mandatory.
    """
    import collections
    injections=collections.Counter()
    for line in injection_path.read_text().splitlines():
        row=json.loads(line); injections[bytes.fromhex(row['hex'])]+=1
    flows={}; connect_syn=0; probes=0
    for stamp,raw in records(pcap):
        guest=raw[6:12]==bytes.fromhex('525400123456')
        if not guest: assert injections[raw]>0; injections[raw]-=1
        p=parse(raw)
        if p is None: continue
        port=p['dest'] if guest else p['source']
        if port==7792:
            assert guest and not p['data']
            assert p['flags']&2 or p['flags']&4
            if p['flags']&2: connect_syn+=1
            continue
        assert port in (7777,7791)
        key=(p['source'],port) if guest else (p['dest'],port)
        flow=flows.setdefault(key,dict(base=[None,None],data=[{},{}],synack=False,window=None))
        d=0 if guest else 1
        if p['flags']&2:
            flow['base'][d]=p['seq'];
            if not guest: flow['synack']=bool(p['flags']&16)
        if port==7791 and not guest:
            assert p['window']==0 and not p['data']; flow['window']=0
        if p['data']:
            assert flow['base'][d] is not None
            at=(p['seq']-flow['base'][d]-1)&0xffffffff
            if port==7791:
                assert guest and flow['window']==0 and len(p['data'])==1 and p['data']==b'Z' and at==0
                probes+=1
            for i,b in enumerate(p['data'],at): assert flow['data'][d].setdefault(i,b)==b
    assert 1<=connect_syn<=4 and probes>=1
    for (_,port),flow in flows.items():
        assert flow['synack']
        if port==7777:
            for data in flow['data']:
                assert len(data)==65536 and bytes(data[i] for i in range(65536))==matrix.BODY
    return dict(connect_syns=connect_syn,zero_window_probes=probes,accepted_bytes_not_delivery=True)

def run(root,mode):
    case=matrix.Case(root,mode,'e1000','socket'); case.config=dict(profiles=['timed-zero-window-send','timed-connect-blackhole'])
    try:
        case.boot(); case.peer=DNSPeer(case.peer); case.client(7777)
        # Use the ordinary peer's constructor hook only for this bounded fixture.
        import net_tcp_socket_peer as peer_module
        old=peer_module.Connection
        peer_module.Connection=ZeroWindow
        try:
            case.peer.listen(7791,b'Z'*8192,b'')
            output=case.finish(case.start('tcpdeadline 10.0.2.2 7791 --send'),8)
            assert 'TCP deadline blocked send / shared fd PASS' in output,output
        finally: peer_module.Connection=old
        output=case.finish(case.start('tcpdeadline 10.0.2.2 7792 --connect'),8)
        assert 'TCP deadline connect PASS' in output,output
        assert '1 probes, 1 replies' in case.finish(case.start('ping -c 1 10.0.2.2'))
        assert '\ntcp-deadline-recovered\n' in case.finish(case.start('echo tcp-deadline-recovered'))
    except BaseException:
        case.failure=traceback.format_exc(); case.diagnostics()
    finally:
        for name in ('test_net_tcp_deadlines.py','net_dns_peer.py'):
            (root/name).write_bytes((matrix.REPO/'scripts'/name).read_bytes())
        case.close()
    print('[PASS] actual timed SEND with zero window/shared fd and unanswered SYN deadline:',root,flush=True)

if __name__=='__main__':
    matrix.audit=deadline_audit
    root=matrix.REPO/'build/net2-step7'/uuid.uuid4().hex[:8]; print('TCP I/O deadline artifacts:',root,flush=True)
    for mode in ('bios','uefi'): run(root/mode,mode)
