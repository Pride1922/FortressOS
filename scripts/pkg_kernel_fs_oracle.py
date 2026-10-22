"""Independent Linux validation of a frozen kernel RAM-device export."""
import hashlib
import re
import subprocess


def audit(path,out):
    before=hashlib.sha256(path.read_bytes()).hexdigest()
    def run(args,label):
        r=subprocess.run(args,text=True,capture_output=True,timeout=60)
        (out/(label+'.log')).write_text(r.stdout+r.stderr);assert r.returncode==0,(args,r.stdout,r.stderr)
        return r.stdout+r.stderr
    run(['e2fsck','-fn',str(path)],'linux-fsck')
    def dump(name,label):
        p=out/(label+'.dump');run(['debugfs','-R',f'dump {name} {p}',str(path)],label);return p.read_bytes()
    ledger=dump('/pkg/grant.v1','ledger');table=dump('/pkg/ownership.v1','ownership')
    assert len(ledger)==1024 and len(table)==8192
    u64=lambda b,o:int.from_bytes(b[o:o+8],'little')
    assert int.from_bytes(ledger[22:24],'little')==1
    assert tuple(u64(ledger,o) for o in (296,304,312,320,328,336))==(8,1,0,0,8,1)
    assert hashlib.sha256(ledger[:992]).digest()==ledger[992:]
    assert hashlib.sha256(table[:8160]).digest()==table[8160:]
    assert hashlib.sha256(table).digest()==ledger[264:296]
    assert dump('/pkg/transaction.v1/unit','payload')==b'K'*32768
    stat=run(['debugfs','-R','stat /pkg/transaction.v1/unit',str(path)],'payload-stat')
    for pattern,value in ((r'Inode:\s*(\d+)',int.from_bytes(table[260:264],'little')),
                          (r'Generation:\s*(\d+)',int.from_bytes(table[264:268],'little')),
                          (r'Size:\s*(\d+)',32768),(r'Blockcount:\s*(\d+)',64),(r'Links:\s*(\d+)',1)):
        assert int(re.search(pattern,stat)[1])==value,(pattern,stat)
    assert re.search(r'User:\s*0\s+Group:\s*0',stat) and re.search(r'Mode:\s*0600',stat)
    for name,expected in (('passwd',b'root:x:0:0:root2:/root:/bin/shell\n'),('group',b'root:x:0:\n'),('shadow',b'root:!:::::::\n')):
        assert dump('/etc/'+name,name)==expected
    assert hashlib.sha256(path.read_bytes()).hexdigest()==before
