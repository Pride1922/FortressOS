"""Finite DNS port-53 fixtures. Protocol helpers are never imported by audit."""
import socket
import struct
import threading
import time
from net_tcp_socket_peer import Peer, Connection, checksum, decode, GUEST_IP, PEER_IP, GUEST_MAC, PEER_MAC

def question(query):
    assert 12 <= len(query) <= 272 and query[2:12] == bytes.fromhex('01000001000000000000')
    labels = []; at = 12
    while query[at]:
        n = query[at]; assert 1 <= n <= 63 and at+1+n < len(query)
        labels.append(query[at+1:at+1+n].decode('ascii')); at += n+1
    assert query[at+1:] == bytes.fromhex('00010001')
    return '.'.join(labels)

def answer(query, tcp=False):
    full_name=question(query); name=full_name.split('.')[0]+'.test'
    header = query[:2] + bytes.fromhex('81800001000100000000')
    tail = bytes.fromhex('c00c000100010000003c00040a000202')
    if name == 'missing.test':
        return query[:2] + bytes.fromhex('81830001000000000000') + query[12:]
    if name == 'alias.test':
        canonical = b'\x07service'+b''.join(bytes([len(label)])+label.encode() for label in full_name.split('.')[1:])+b'\0'
        tail = bytes.fromhex('c00c000500010000003c') + len(canonical).to_bytes(2, 'big') + canonical
    if name in ('tcp.test', 'stall.test', 'trickle.test', 'eof.test') and not tcp:
        return query[:2] + bytes.fromhex('83800001000100000000') + query[12:]
    if name == 'bad.test': tail = bytes.fromhex('c00c000100010000003c00030a0002')
    assert name in ('service.test','alias.test','missing.test','tcp.test','stall.test','trickle.test','eof.test','bad.test','timeout.test')
    return header + query[12:] + tail

def udp_frame(port, data):
    udp = struct.pack('!HHHH',53,port,len(data)+8,0)+data
    value=checksum(PEER_IP+GUEST_IP+struct.pack('!BBH',0,17,len(udp))+udp) or 65535
    udp=udp[:6]+value.to_bytes(2,'big')+udp[8:]
    ip=struct.pack('!BBHHHBBH4s4s',0x45,0,len(udp)+20,0x5678,0x4000,64,17,0,PEER_IP,GUEST_IP)
    ip=ip[:10]+checksum(ip).to_bytes(2,'big')+ip[12:]
    raw=GUEST_MAC+PEER_MAC+b'\x08\x00'+ip+udp
    return raw+bytes(max(0,60-len(raw)))

class DNSConnection(Connection):
    def __init__(self, peer, packet, query):
        self.name=question(query).split('.')[0]+'.test'
        expected=len(query).to_bytes(2,'big')+query
        body=answer(query,True); response=len(body).to_bytes(2,'big')+body
        if self.name=='stall.test': response=b''
        if self.name=='trickle.test': response=response[:10]
        if self.name=='eof.test': response=response[:1]
        super().__init__(peer,53,expected,response,guest_port=packet['source'],profile='dns',mss=536)
        self.next_byte=0
    def pump(self):
        if self.state=='ESTABLISHED' and len(self.consumed)==len(self.expected):
            position=self.next-1
            if position<len(self.response) and time.monotonic()>=self.next_byte and self.window>self.next-self.una:
                self.send(24,self.response[position:position+1],True)
                self.next_byte=time.monotonic()+(.7 if self.name=='trickle.test' else .01)
            if not self.own_fin and self.una==self.next and (
                self.remote_fin or (self.name=='eof.test' and position>=len(self.response))):
                self.own_fin=True; self.send(17,retain=True)
        # Retry/control handling from the transport peer; never its app pump.
        state=self.state
        if state=='ESTABLISHED': self.state='DNS_WAIT'
        super().pump()
        if self.state=='DNS_WAIT': self.state=state

class Tap:
    def __init__(self, peer, sock): self.peer,self.sock=peer,sock
    def recv(self,n):
        raw=self.sock.recv(n)
        if len(raw)>=42 and raw[12:14]==b'\x08\x00' and raw[23]==1:
            length=int.from_bytes(raw[16:18],'big'); body=raw[34:14+length]
            assert checksum(raw[14:34])==0 and checksum(body)==0
            assert raw[26:30]==GUEST_IP and raw[30:34]==PEER_IP
            if body[0]==8:
                assert body[1]==0
                reply=b'\0\0\0\0'+body[4:]
                reply=reply[:2]+checksum(reply).to_bytes(2,'big')+reply[4:]
                ip=struct.pack('!BBHHHBBH4s4s',0x45,0,20+len(reply),0x5679,0x4000,64,1,0,PEER_IP,GUEST_IP)
                ip=ip[:10]+checksum(ip).to_bytes(2,'big')+ip[12:]
                response=GUEST_MAC+PEER_MAC+b'\x08\x00'+ip+reply
                self.peer.inject(response+bytes(max(0,60-len(response))))
            return b''
        if len(raw)>=42 and raw[12:14]==b'\x08\x00' and raw[23]==17:
            length=int.from_bytes(raw[16:18],'big'); segment=raw[34:14+length]
            assert checksum(raw[14:34])==0 and checksum(raw[26:34]+b'\0\x11'+len(segment).to_bytes(2,'big')+segment)==0
            source,dest,size,_=struct.unpack('!HHHH',segment[:8])
            if dest==53:
                assert size==len(segment)
                q=segment[8:]; name=question(q).split('.')[0]+'.test'; self.peer.last_query=q
                self.peer.event('dns-query',dns_name=name,query=q.hex(),port=source)
                if name!='timeout.test': self.peer.inject(udp_frame(source,answer(q)))
            return b''
        packet=decode(raw)
        if packet and packet['dest']==53 and packet['flags']&2:
            if not any(c.guest_port==packet['source'] and c.port==53 for c in self.peer.connections):
                assert self.peer.last_query is not None and len(self.peer.connections)<8
                self.peer.connections.append(DNSConnection(self.peer,packet,self.peer.last_query))
        return raw
    def sendto(self,*args): return self.sock.sendto(*args)
    def setblocking(self,*args): return self.sock.setblocking(*args)

class DNSPeer(Peer):
    def __init__(self,previous):
        super().__init__(previous.sock,previous.target,previous.injections,previous.events)
        self.last_query=None; self.sock=Tap(self,self.sock)

class HostDNS:
    """Real loopback UDP/TCP 53 for SLIRP. Requires bind permission; no escalation."""
    def __init__(self):
        self.stop=threading.Event(); self.errors=[]; self.threads=[]; self.streams=[]
        self.udp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        self.tcp=socket.socket(); self.tcp.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
        try:
            self.udp.bind(('127.0.0.1',53)); self.tcp.bind(('127.0.0.1',53)); self.tcp.listen(4)
        except BaseException:
            self.udp.close(); self.tcp.close(); raise
        self.udp.settimeout(.1); self.tcp.settimeout(.1)
        self.launch(self.udp_loop); self.launch(self.tcp_loop)
    def launch(self,fn,*args):
        def wrapped():
            try: fn(*args)
            except Exception as exc:
                if not self.stop.is_set(): self.errors.append(repr(exc))
        t=threading.Thread(target=wrapped,daemon=True); self.threads.append(t); t.start()
    def udp_loop(self):
        while not self.stop.is_set():
            try: q,addr=self.udp.recvfrom(1472)
            except socket.timeout: continue
            if question(q).split('.')[0]!='timeout': self.udp.sendto(answer(q),addr)
    def tcp_loop(self):
        while not self.stop.is_set():
            try: s,_=self.tcp.accept()
            except socket.timeout: continue
            assert len(self.threads)<64; self.launch(self.client,s)
    def client(self,s):
        with s:
            s.settimeout(40)
            def exact(n):
                b=b''
                while len(b)<n:
                    p=s.recv(n-len(b)); assert p; b+=p
                return b
            q=exact(int.from_bytes(exact(2),'big')); full_name=question(q); name=full_name.split('.')[0]+'.test'
            body=answer(q,True); wire=len(body).to_bytes(2,'big')+body
            if name=='stall.test': wire=b''
            if name=='trickle.test': wire=wire[:10]
            if name=='eof.test': wire=wire[:1]
            self.streams.append(dict(port=53,guest_port=None,expected=(len(q).to_bytes(2,'big')+q).hex(),response=wire.hex(),active=False,dns_name=full_name))
            for byte in wire:
                s.sendall(bytes([byte])); time.sleep(.7 if name=='trickle.test' else .01)
            if name=='eof.test': s.shutdown(socket.SHUT_WR)
            while s.recv(4096): pass
    def close(self):
        self.stop.set(); self.udp.close(); self.tcp.close()
        for t in self.threads: t.join(timeout=1)
        assert not self.errors,self.errors
