"""Targeted unsupported authorized archive regression; no shared build target."""
import argparse,datetime,json,hashlib,pathlib,os,subprocess
from pkg_mount_vectors import vectors
from test_pkg_enrollment import signed,policy
ROOT=pathlib.Path(__file__).resolve().parents[1]
def main():
 ap=argparse.ArgumentParser();ap.add_argument('evidence',type=pathlib.Path);ap.add_argument('--expect-assert',action='store_true');ap.add_argument('--foreign-genesis',action='store_true');args=ap.parse_args();base=args.evidence.resolve();m=json.loads((base/'manifest.json').read_text());probe=base/'probe'
 seed=next(c for c in m['commands'] if c[0]==str(probe) and c[1]=='setup');image,ss=seed[2:4]
 out=ROOT/'.codex-remote-attachments'/('pkg-mount-unsupported-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'));out.mkdir();inputs=out/'inputs';extra=vectors(inputs,with_doc=not args.foreign_genesis,foreign_genesis=args.foreign_genesis);env=dict(os.environ,PKG_MOUNT_INPUT=str(inputs),PKG_MOUNT_PHASE='2',PKG_MOUNT_PUBLICATION='2' if args.foreign_genesis else '0');prefix=out/'unsupported'
 def run(cmd,label):
  r=subprocess.run(cmd,cwd=base/'workspace',env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=180);(out/(label+'.log')).write_text(r.stdout);return r
 r=run([str(probe),'setup',image,ss,str(prefix)],'setup');assert not r.returncode,r.stdout
 body=pathlib.Path(str(prefix)+'-body.img').read_bytes();desc=signed(body);en=out/'enrollment.bin';en.write_bytes(desc);pol=out/'policy.bin';pol.write_bytes(policy(desc,uuids=[body[40+16*i:56+16*i] for i in range(5)])+extra)
 r=run([str(probe),'reject',str(prefix)+'-setup.img',ss,str(en),str(pol),str(out/'reject')],'reject')
 expected=(r.returncode!=0 and ('UNEXPECTED acceptance' in r.stdout if args.foreign_genesis else 'ctx->verify.verified' in r.stdout)) if args.expect_assert else r.returncode==0 and 'PASS reject writes=0 flushes=0' in r.stdout
 result={'status':'REPRODUCED_ASSERTION' if args.expect_assert and expected else 'PASS' if expected else 'FAIL','base':str(base),'base_manifest_sha256':hashlib.sha256((base/'manifest.json').read_bytes()).hexdigest(),'probe_sha256':hashlib.sha256(probe.read_bytes()).hexdigest(),'returncode':r.returncode,'expected_assert':args.expect_assert,'foreign_genesis':args.foreign_genesis,'scope':('Consumed-source pending state with foreign genesis must reject before replay' if args.foreign_genesis else 'Valid signed two-file artifact rejected by single-file profile; tests verified-bit clearing and constructor unwind')}
 (out/'manifest.json').write_text(json.dumps(result,indent=2)+'\n');print('Evidence:',out,result['status']);assert expected,r.stdout
if __name__=='__main__':main()
