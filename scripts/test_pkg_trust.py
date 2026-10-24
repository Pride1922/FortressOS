"""Private trust checkpoint pure actual-code ASan/UBSan and freestanding gate."""
import datetime,json,pathlib,struct,subprocess,tempfile
from pkg_enroll_records import H,PUB,keys
from pkg_trust_records import certificate
ROOT=pathlib.Path(__file__).resolve().parents[1]
def main():
    out=ROOT/'.codex-remote-attachments'/('pkg-trust-host-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'));out.mkdir();m={'runtime':'INCOMPLETE','boot_acceptance':'NOT RUN','commands':[],'cases':[]}
    def run(cmd,label):
        m['commands'].append(cmd);p=subprocess.run(cmd,cwd=ROOT,capture_output=True,text=True);(out/(label+'.log')).write_text(p.stdout+p.stderr);assert not p.returncode,(label,p.returncode,p.stderr[-3000:]);m['cases'].append(label)
    try:
        flags=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I.','-Itests','-Itests/host','-Isrc/include','-Isrc/lib','-Isrc/lib/crypto/upstream/src']
        objs=[]
        for i,src in enumerate(('src/lib/crypto/upstream/src/monocypher.c','src/lib/crypto/upstream/src/optional/monocypher-ed25519.c')):
            p=out/f'mono{i}.o';run(flags+['-include','src/lib/crypto/namespace.h','-c',src,'-o',str(p)],f'crypto-{i}');objs.append(str(p))
        native=out/'trust-host';run(flags+['tests/pkg_trust_host.c','tests/pkg_trust_floor.c','src/lib/crypto/hash.c','src/lib/crypto/curve.c','user/tools/digest.c',*objs,'-o',str(native)],'compile')
        run(['gcc','-std=c11','-Os','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-mno-red-zone','-mno-sse','-mno-mmx','-msoft-float','-fstack-usage','-Wframe-larger-than=1024','-I.','-Itests','-Isrc/include','-Isrc/lib','-c','tests/pkg_trust_floor.c','-o',str(out/'trust-free.o')],'freestanding')
        with tempfile.TemporaryDirectory(prefix='fortress-trust-') as td:
            td=pathlib.Path(td);paths=[]
            for i in (111,112,113):p=td/str(i);p.write_bytes(bytes([i])*32);p.chmod(0o600);paths.append(p)
            signers=keys(paths);policy=bytearray(1136)
            for i,k in enumerate(signers):policy[i*32:(i+1)*32]=PUB(k)
            struct.pack_into('<6I',policy,768,3,2,0,0,0,0);policy[792:1096]=bytes((i%251+1 for i in range(304)));struct.pack_into('<5Q',policy,1096,1792843200,5,4,8,2)
            loc=bytearray(128);struct.pack_into('<HHIIIQQH',loc,0,49,1,25,990,0o600,1024,1,1);struct.pack_into('<IIQ',loc,40,0,1,999)
            h=(H(b'root4'),H(b'catalog8'),bytes(32));records=[certificate(policy,loc,1,(3,7,0,1,1),(H(b'root3'),H(b'catalog7'),bytes(32)),signers),certificate(policy,loc,2,(4,8,0,1,1),h,signers),certificate(policy,loc,3,(4,8,1,7,2),(*h[:2],H(b'artifact')),signers)]
            (out/'policy').write_bytes(policy);wire=out/'records';wire.write_bytes(b''.join(records));run([str(native),str(out/'policy'),str(wire),'1','unused'],'valid-and-adversarial')
            for label,offset in (('reserved',400),('locator',488),('version',20),('identity',40),('padding',900),('signature',700),('zero-epoch',24),('zero-contract',32),('locator-hash',448)):
                bad=bytearray(records[0]);bad[offset]^=1
                if label=='zero-contract':struct.pack_into('<I',bad,32,0)
                if offset<608:
                    bad[608:640]=H(bad[:608]);from pkg_enroll_records import signed
                    bad=bytearray(signed(bad[:640],signers).ljust(1024,b'\0'))
                wire.write_bytes(bytes(bad)+b''.join(records[1:]));run([str(native),str(out/'policy'),str(wire),'0','unused'],label)
            wrong=[]
            for i in (1,2,3):p=td/str(i);p.write_bytes(bytes([i])*32);p.chmod(0o600);wrong.append(p)
            from pkg_enroll_records import signed
            wire.write_bytes(signed(records[0][:640],keys(wrong)).ljust(1024,b'\0')+b''.join(records[1:]));run([str(native),str(out/'policy'),str(wire),'0','unused'],'wrong-role-signatures')
        m['runtime']='PASS'
    except BaseException as e:m['runtime']='FAIL';m['error']=repr(e);raise
    finally:(out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n');print(out,m['runtime'],flush=True)
if __name__=='__main__':main()
