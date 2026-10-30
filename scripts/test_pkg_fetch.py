"""Actual normal fetch frontend plus C verifiers, mocked network/file delivery.

Ephemeral TEST keys and unsigned synthetic ELF only; no provisioning, live
network, filesystem install, physical disks or boot-media build.
"""
import copy
import datetime
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import uuid
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from pkg_repository import catalog,public
from pkg_artifact_vectors import pack,parts
NOW=1793188800

ROOT=Path(__file__).resolve().parents[1]


def main():
    out=ROOT/".codex-remote-attachments"/("pkg-fetch-"+datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H%M%S-%f"))
    out.mkdir(parents=True);report={"runtime":"INCOMPLETE","boot":"NOT RUN","network":"MOCKED","commands":[]}
    units=["tests/pkg_fetch_host.c",*[str(p.relative_to(ROOT)) for p in sorted((ROOT/"user/pkg").glob("*.c"))],
           "src/lib/crypto/hash.c","src/lib/crypto/curve.c",
           "user/tools/common.c","user/tools/digest.c","user/tools/gzip.c"]
    paths=units+["scripts/test_pkg_fetch.py","user/pkg/repository.h"]
    before={p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in paths}
    def run(command):
        index=len(report["commands"]);report["commands"].append(command)
        result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True,timeout=180)
        (out/f"command-{index}.log").write_text(result.stdout+result.stderr)
        assert result.returncode==0,(result.stdout[-2000:],result.stderr[-3000:])
    try:
        keys=[Ed25519PrivateKey.generate() for _ in range(2)]
        artifact=pack(*parts());repository=bytes(range(16))
        wire,_=catalog(repository,1,5,NOW-1,NOW+1000,[artifact],keys)
        spec={'repository':str(uuid.UUID(bytes=repository)),'root_version':1,'platform':bytes(range(32)).hex(),'base_version':2,
              'keys':[public(key).hex() for key in keys]}
        flags=["gcc","-std=c11","-O1","-g","-Wall","-Wextra","-Werror","-no-pie","-fsanitize=address,undefined","-fno-omit-frame-pointer",
               "-ffunction-sections","-fdata-sections","-Wl,--gc-sections","-DTOOL_HOST_TEST","-DFORTRESS_PKG_FETCH=1","-DFORTRESS_PKG_TRUST=1",
               "-I.","-Isrc/include","-Isrc/fs","-Isrc/kernel","-Iuser/tools","-Isrc/lib/crypto/upstream/src"]
        objects=[]
        for index,source in enumerate(("src/lib/crypto/upstream/src/monocypher.c","src/lib/crypto/upstream/src/optional/monocypher-ed25519.c")):
            obj=str(out/f"crypto-{index}.o");run(flags+["-include","src/lib/crypto/namespace.h","-c",source,"-o",obj]);objects.append(obj)
        run(flags+units+objects+["-o",str(out/"probe")])
        (out/"catalog.v1").write_bytes(wire);(out/"artifact.fpk").write_bytes(artifact)
        def case(label,policy_spec,expected_downloads=None):
            text='version=1\n'+''.join(f'{name}={policy_spec[name]}\n' for name in ('repository','root_version','platform','base_version'))
            text+=''.join('public_key='+key+'\n' for key in policy_spec['keys'])
            pf=out/(label+'.trust.conf');pf.write_text(text)
            run([str(out/"probe"),str(pf),str(out/"catalog.v1"),str(out/"artifact.fpk"),*([str(expected_downloads)] if expected_downloads is not None else [])])
        case("valid",spec)
        wrong=copy.deepcopy(spec);wrong['keys']=[public(Ed25519PrivateKey.generate()).hex()];case('wrong-key',wrong,1)
        wrong=copy.deepcopy(spec);wrong['platform']=hashlib.sha256(b'TEST other platform').hexdigest();case('wrong-platform',wrong,2)
        report["runtime"]="PASS"
    except BaseException as error:report["runtime"]="FAIL";report["error"]=repr(error);raise
    finally:
        report["sources"]=before;report["preserved"]=before=={p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in paths}
        if not report["preserved"]:report["runtime"]="FAIL"
        (out/"manifest.json").write_text(json.dumps(report,indent=2)+"\n");print(out,report["runtime"],flush=True)
    assert report["runtime"]=="PASS"


if __name__=="__main__":main()
