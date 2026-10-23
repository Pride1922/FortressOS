"""Second fresh-process recovery of every committed grant-preview state."""
import argparse,datetime,hashlib,json,os,pathlib,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 ap=argparse.ArgumentParser();ap.add_argument('evidence',nargs='+',type=pathlib.Path);args=ap.parse_args();out=ROOT/'.codex-remote-attachments'/('pkg-mount-repeat-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'));out.mkdir();result={'status':'INCOMPLETE','cases':[],'bases':[],'commands':[]}
 def run(cmd,cwd,env,label):
  result['commands'].append(cmd);r=subprocess.run(cmd,cwd=cwd,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=180);(out/(label+'.log')).write_text(r.stdout);assert not r.returncode,(label,r.returncode,r.stdout[-2000:]);return r.stdout
 try:
  for supplied in args.evidence:
   base=supplied.resolve();m=json.loads((base/'manifest.json').read_text());assert len(m['cases'])==14 and all(c['status']=='PASS' for c in m['cases']);manifest_hash=sha(base/'manifest.json');probe_hash=sha(base/'probe');result['bases'].append({'path':str(base),'manifest_sha256':manifest_hash,'probe_sha256':probe_hash})
   for case in m['cases']:
    bs,ss,phase,pub=[case[k] for k in ('block','sector','phase','publication')];tag=f'{bs}-{ss}-p{phase}-v{pub}';img=base/(tag+'-journal-final.img');before=sha(img);prefix=out/tag;env=dict(os.environ,PKG_MOUNT_INPUT=str(base/'fixture-inputs'),PKG_MOUNT_PHASE=str(phase),PKG_MOUNT_PUBLICATION=str(pub))
    text=run([str(base/'probe'),'recover',str(img),str(ss),str(base/(tag+'-enrollment.bin')),str(base/(tag+'-policy.bin')),str(prefix)],base/'workspace',env,tag)
    assert 'grant=2 newer=0' in text and 'commits=0' in text
    final=pathlib.Path(str(prefix)+'-final.img');run(['e2fsck','-fn',str(final)],base/'workspace',env,tag+'-fsck')
    for name in ('grant.v1','ownership.v1','state.v1','trust.v1','account-generation.v1','origin.v1'):
     target=out/(tag+'-'+name+'.dump');run(['debugfs','-R',f'dump /pkg/{name} {target}',str(final)],base/'workspace',env,tag+'-'+name)
     assert target.read_bytes()==(base/(tag+'-journal-'+name+'.dump')).read_bytes()
    assert sha(img)==before;result['cases'].append({'tag':tag,'input_sha256':before,'final_sha256':sha(final),'status':'PASS'});final.unlink()
   assert sha(base/'manifest.json')==manifest_hash and sha(base/'probe')==probe_hash
   assert all(sha(base/'workspace'/p)==h for p,h in m['compiled_inputs'].items())
  result['status']='PASS'
 except Exception as exc:result.update(status='FAIL',error=repr(exc));raise
 finally:
  result['artifacts']={p.name:sha(p) for p in out.iterdir() if p.is_file()};(out/'manifest.json').write_text(json.dumps(result,indent=2)+'\n');print('Evidence:',out,result['status'],len(result['cases']))
 assert result['status']=='PASS'
if __name__=='__main__':main()
