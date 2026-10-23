"""Private retirement-ledger codec and coupled accounting, before mount wiring.

All atomic publication/CAS tests are explicitly a host model, not JBD2 tests.
"""
import copy
import datetime
import hashlib
import json
import pathlib
import re
import shutil
import struct
import subprocess

ROOT=pathlib.Path(__file__).resolve().parents[1]
H=lambda b:hashlib.sha256(b).digest()
def put(b,o,v,fmt='Q'):struct.pack_into('<'+fmt,b,o,v)
def seal(b):b[-32:]=H(b[:-32])
def record(magic,version=1):
    b=bytearray(1024);b[:16]=magic.encode().ljust(16,b'\0');put(b,16,1024,'I');put(b,20,version,'H');return b
def rehash(b):
    m,g,t,l,a,o=b
    seal(m);seal(t);g[264:296]=H(t);seal(g)
    l[136:168]=H(g);l[168:200]=H(t);seal(l)
    seal(o);a[112:144]=H(o);a[448:480]=H(g);a[592:624]=H(l);seal(a)
def fixture(blocks=4,cls=1,flags=1,seq=10,account_seq=50):
    m=bytearray(512);m[:16]=b'FORT-PKG-ENR-v1\0';put(m,16,512,'I');put(m,20,1,'H');put(m,24,1)
    for i in range(5):m[32+16*i:48+16*i]=bytes([i+1])*16
    m[112:144]=b'P'*32
    for i in range(3):struct.pack_into('<II',m,144+8*i,44+i,1)
    put(m,168,2,'I');put(m,172,1,'I')
    g=record('FORT-PKG-GRT-v1');put(g,22,3,'H');put(g,24,seq);put(g,32,1);g[40:56]=b'T'*16
    g[56:72]=m[96:112];g[72:88]=m[32:48];g[88:104]=m[80:96];g[104:136]=m[112:144]
    for off,ch in ((136,b'I'),(168,b'O'),(200,b'N'),(232,b'U'),(392,b'B')):g[off:off+32]=ch*32
    put(g,344,1,'I');put(g,348,1,'H');put(g,352,46,'I');put(g,356,1,'I')
    for off,value in ((360,1),(368,2),(376,3),(384,7)):put(g,off,value)
    t=bytearray(8192);t[:16]=b'FORT-PKG-OWN-v1\0';put(t,16,8192,'I');put(t,20,1,'H');put(t,22,2,'H');put(t,24,1)
    t[32:48]=g[40:56];put(t,48,seq);t[56:136]=g[56:136]
    for index,role,ino,cl,fl,size,owned in ((0,1,100,1,1,2048,2),(1,2,101,cls,flags,0 if cls==3 else 4096,blocks)):
        at=256+64*index;struct.pack_into('<HHIII',t,at,role,cl,ino,7,fl);t[at+16:at+32]=g[40:56]
        struct.pack_into('<QQQ',t,at+32,size,owned,0 if cl==3 else 1)
    q=(2+blocks,1+(cls==1));total=(16,6);remaining=tuple(total[i]-q[i] for i in range(2))
    for i in range(2):put(g,296+8*i,total[i]);put(g,312+8*i,remaining[i]);put(g,328+8*i,q[i])
    l=record('FORT-PKG-RLG-v1');put(l,24,seq);put(l,32,1);l[40:104]=g[40:104];l[104:136]=b'C'*32
    put(l,200,account_seq);put(l,208,9);struct.pack_into('<HHIIIQ',l,216,2,cls,flags,101,7,0 if cls==3 else 4096)
    for i in range(2):put(l,240+8*i,total[i]);put(l,256+8*i,remaining[i]);put(l,272+8*i,q[i]);put(l,288+8*i,(blocks,0 if cls==3 else 1)[i])
    a=record('FORT-ACC-GEN-v1',4);put(a,24,account_seq);put(a,32,4);put(a,40,1);a[48:64]=m[96:112];a[64:80]=m[32:48];a[80:112]=b'A'*32;put(a,144,9)
    for i in range(3):a[328+32*i:360+32*i]=bytes([10+i])*32;put(a,424+4*i,32,'I')
    put(a,440,seq);a[480:512]=g[232:264];a[512:544]=g[200:232];a[544:576]=g[136:168];a[576:592]=g[40:56]
    o=record('FORT-ACC-ORG-v1');put(o,24,account_seq);put(o,32,1);put(o,40,4);o[48:112]=a[48:112]
    b=[m,g,t,l,a,o];rehash(b);return b
def context(b):
    m=b[0];p=m[32:144]+H(m)+m[24:32]+m[144:168]+b'C'*32+b'A'*32+struct.pack('<HHII',2,0,101,7)+b'T'*16+b'U'*32+b'N'*32+b'I'*32
    assert len(p)==364;return p

def main():
    stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H%M%S-%f')
    out=ROOT/'.codex-remote-attachments'/('pkg-retirement-codec-'+stamp);work=out/'workspace';work.mkdir(parents=True)
    paths=['tests/pkg_retirement_candidate.c','tests/pkg_retirement_candidate.h','tests/pkg_retirement_host.c','tests/pkg_grant_candidate.c','tests/pkg_grant_candidate.h','user/tools/digest.c','user/tools/digest.h','src/include/types.h','scripts/test_pkg_retirement.py','tests/pkg_grant_candidate.mk','AGENTS.md','README.md']
    sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    before={p:sha(ROOT/p) for p in paths}
    guards={str(p.relative_to(ROOT)):sha(p) for p in (ROOT/'bin').iterdir() if p.name in ('fortress.elf','initramfs.tar','fortress.iso','fortress.img')}
    for p in paths:
        dst=work/p;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(ROOT/p,dst)
    manifest={'status':'INCOMPLETE','sources':before,'guards':guards,'commands':[],'validation_cases':[],'transition_cases':[],
              'scope':'pure private ledger/account-v4 codec, accounting transitions and mock atomic generation/CAS; no mount, filesystem, JBD2, signature, boot or installation integration'}
    lines=[];serial=0
    def save(b,label,ctx=None):
        nonlocal serial
        serial+=1;name=f'{serial:04d}-{label}.bin';(out/name).write_bytes((context(b) if ctx is None else ctx)+b''.join(b));return name
    def validate(b,label,expected=0,ctx=None):
        name=save(b,label,ctx);lines.append(f'V {expected} {label} {name}\n');manifest['validation_cases'].append(dict(label=label,expected=expected))
    def transition(a,b,label,expected=0,op=2,f=(27,12),next=(27,12),mixed=False):
        ap=save(a,label+'-before');bp=save(b,label+'-after');lines.append(f'{"M" if mixed else "T"} {expected} {op} {f[0]} {f[1]} {next[0]} {next[1]} {label} {ap} {bp}\n');manifest['transition_cases'].append(dict(label=label,expected=expected,operation=op,mixed=mixed))
    chain=[fixture(),fixture(flags=2,seq=11,account_seq=51),fixture(blocks=3,flags=2,seq=12,account_seq=52),fixture(blocks=0,flags=2,seq=13,account_seq=53),fixture(blocks=0,cls=3,flags=0,seq=14,account_seq=54)]
    for i,b in enumerate(chain):validate(b,'initial' if i==0 else 'state-'+str(i),1)
    observations=[(27,12),(27,12),(28,12),(31,12),(31,13)]
    for i in range(4):transition(chain[i],chain[i+1],'step-'+str(i),1,1 if i==0 else 2,observations[i],observations[i+1],True)
    transition(chain[-1],chain[-1],'replay',1,0,observations[-1],observations[-1])
    transition(chain[-1],chain[-1],'replay-invented-free',next=(32,13),f=(31,13))
    transition(chain[-1],chain[-1],'replay-underfunded',next=(0,0),f=(0,0))
    # Rehashed field defects isolate grammar and binding rather than checksum.
    for record_index,off,value,fmt in [(3,16,1023,'I'),(3,20,2,'H'),(3,22,1,'H'),(3,24,0,'Q'),(3,32,0,'Q'),(3,200,0,'Q'),(3,208,0,'Q'),(3,216,0,'H'),(3,216,21,'H'),(3,218,2,'H'),(3,220,3,'I'),(3,224,0,'I'),(3,228,0,'I'),(3,240,17,'Q'),(3,256,2**64-1,'Q'),(3,288,7,'Q'),(3,296,2,'Q'),(4,20,3,'H'),(4,22,1,'H'),(4,24,51,'Q'),(4,32,0,'Q'),(4,144,10,'Q'),(4,152,1,'Q'),(4,160,1,'H'),(4,436,1,'I'),(4,440,11,'Q'),(4,624,1,'B'),(5,22,1,'H'),(5,24,51,'Q'),(5,40,5,'Q'),(5,112,1,'B')]:
        b=copy.deepcopy(chain[0]);put(b[record_index],off,value,fmt);rehash(b);validate(b,f'field-{record_index}-{off}')
    for record_index,off,n in [(3,40,16),(3,56,16),(3,72,16),(3,88,16),(3,104,32),(4,48,16),(4,64,16),(4,80,32),(4,328,32),(4,480,32),(4,512,32),(4,544,32),(4,576,16),(5,48,16),(5,64,16),(5,80,32)]:
        b=copy.deepcopy(chain[0]);b[record_index][off:off+n]=bytes(n);rehash(b);validate(b,f'zero-{record_index}-{off}')
    for off in (0,80,112,144,152,176,208,240,244,248,252,268,300,332):
        b=chain[0];ctx=bytearray(context(b));ctx[off]^=1;validate(b,'external-pin-'+str(off),ctx=ctx)
    for off,account_off in ((232,480),(200,512),(136,544)):
        b=copy.deepcopy(chain[0]);b[1][off]^=1;b[4][account_off:account_off+32]=b[1][off:off+32];rehash(b);validate(b,'coherent-authorization-'+str(off))
    b=copy.deepcopy(chain[0]);b[1][40:56]=b'X'*16;b[2][32:48]=b'X'*16;b[2][272:288]=b'X'*16;b[2][336:352]=b'X'*16;b[3][40:56]=b'X'*16;b[4][576:592]=b'X'*16;rehash(b);validate(b,'coherent-foreign-transaction')
    b=copy.deepcopy(chain[0]);put(b[1],348,2,'H');rehash(b);validate(b,'unsupported-operation')
    b=copy.deepcopy(chain[0]);put(b[1],424,1);rehash(b);validate(b,'unsupported-completed-step')
    for part,ranges in ((4,((152,328),(436,440),(624,992))),(5,((112,992),))):
        for start,end in ranges:
            for off in range(start,end):
                b=copy.deepcopy(chain[0]);b[part][off]=1;rehash(b);validate(b,f'rehashed-padding-{part}-{off}')
    # Fully valid endpoints whose transition authority is wrong.
    for i in range(4):
        a,b=chain[i:i+2];f,n=observations[i:i+2]
        for dim in range(2):
            for delta in (-1,1):
                wrong=list(n);wrong[dim]+=delta;transition(a,b,f'free-{i}-{dim}-{delta}',f=f,next=wrong)
    transition(chain[0],fixture(blocks=0,cls=3,flags=0,seq=11,account_seq=51),'linked-direct-tombstone',f=observations[0],next=observations[-1])
    transition(chain[1],fixture(blocks=0,cls=3,flags=0,seq=12,account_seq=52),'skip-data-retirement',f=(27,12),next=(31,13))
    transition(chain[1],fixture(flags=2,seq=12,account_seq=52),'zero-refund-advance')
    transition(chain[1],fixture(flags=1,seq=12,account_seq=52),'relink')
    transition(chain[1],fixture(blocks=5,flags=2,seq=12,account_seq=52),'regrow',next=(26,12))
    transition(chain[0],fixture(flags=2,seq=12,account_seq=51),'skip-grant-generation')
    transition(chain[0],fixture(flags=2,seq=11,account_seq=52),'skip-account-generation')
    transition(fixture(seq=2**64-1),fixture(flags=2,seq=1,account_seq=51),'exhausted-grant')
    transition(fixture(account_seq=2**64-1),fixture(flags=2,seq=11,account_seq=1),'exhausted-account')
    for label,mutate in [('canonical-account',lambda b:b[4].__setitem__(328,99)),('credential-generation',lambda b:(put(b[4],144,10),put(b[3],208,10))),('operation-generation',lambda b:(put(b[4],32,5),put(b[5],40,5))),('other-owned-inode',lambda b:put(b[2],264,8,'I'))]:
        b=copy.deepcopy(chain[1]);mutate(b);rehash(b);validate(b,'valid-endpoint-'+label,1);transition(chain[0],b,label,op=1)
    for delta in (-1,1):
        b=copy.deepcopy(chain[1])
        for off in (296,312):put(b[1],off,struct.unpack_from('<Q',b[1],off)[0]+delta)
        for off in (240,256):put(b[3],off,struct.unpack_from('<Q',b[3],off)[0]+delta)
        rehash(b);validate(b,'valid-total-change-'+str(delta),1);transition(chain[0],b,'unauthorized-total-change-'+str(delta),op=1)
    b=copy.deepcopy(chain[1]);put(b[2],296,3);put(b[1],328,7);put(b[1],312,9);put(b[3],272,7);put(b[3],256,9);rehash(b)
    validate(b,'valid-other-object-charge',1);transition(chain[0],b,'unrelated-object-charge',op=1)
    (out/'cases.txt').write_text(''.join(lines))
    def run(cmd,label):
        manifest['commands'].append(cmd);r=subprocess.run(cmd,cwd=work if label.startswith('compile') else out,capture_output=True,text=True,timeout=120)
        (out/(label+'.log')).write_text(r.stdout+r.stderr);assert r.returncode==0,(label,r.returncode,r.stderr[-2000:]);return r.stdout
    flags=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-I.','-Itests','-Isrc/include','-Iuser/tools','-ffunction-sections','-fdata-sections']
    try:
        units=['tests/pkg_retirement_candidate.c','tests/pkg_grant_candidate.c','tests/pkg_retirement_host.c','user/tools/digest.c']
        run(flags+['-fsanitize=address,undefined','-fno-omit-frame-pointer','-fstack-usage','-no-pie','-Wl,--gc-sections',*units,'-o',str(out/'probe')],'compile-host')
        report=run([str(out/'probe'),'cases.txt'],'host');print(report,end='')
        manifest['results']={k:int(v) for k,v in re.findall(r'([a-z_]+)=(\d+)',report)}
        run(flags+['-O2','-ffreestanding','-fno-builtin','-mno-red-zone','-mno-sse','-mno-mmx','-msoft-float','-fstack-usage','-Wframe-larger-than=1024','-c','tests/pkg_retirement_candidate.c','-o',str(out/'candidate.o')],'compile-freestanding')
        manifest['status']='PASS'
    except Exception as e:manifest['status']='FAIL';manifest['error']=repr(e);raise
    finally:
        manifest['changed_sources']=[p for p,h in before.items() if sha(ROOT/p)!=h]
        manifest['changed_copies']=[p for p,h in before.items() if sha(work/p)!=h]
        manifest['changed_media']=[p for p,h in guards.items() if sha(ROOT/p)!=h]
        if manifest['changed_sources'] or manifest['changed_copies'] or manifest['changed_media']:manifest['status']='FAIL'
        manifest['artifacts']={p.name:sha(p) for p in out.iterdir() if p.is_file() and p.name!='manifest.json'}
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');print('Evidence:',out,manifest['status'],flush=True)
    assert manifest['status']=='PASS'
if __name__=='__main__':main()
