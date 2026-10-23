"""Enumerate every recovery commit from retained-temp and pre-intent states."""
import argparse,datetime,hashlib,json,pathlib,re,subprocess
from concurrent.futures import ThreadPoolExecutor
ROOT=pathlib.Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 ap=argparse.ArgumentParser();ap.add_argument('evidence',type=pathlib.Path);args=ap.parse_args();base=args.evidence.resolve();m=json.loads((base/'manifest.json').read_text());work=base/'workspace'
 out=ROOT/'.codex-remote-attachments'/('pkg-account-service-redo-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'));out.mkdir()
 result={'status':'INCOMPLETE','base':str(base),'base_manifest_sha256':sha(base/'manifest.json'),'probe_sha256':sha(base/'probe'),'commands':[],'cases':[]}
 def run(cmd,label):
  result['commands'].append(cmd)
  with (out/(label+'.log')).open('w') as log:r=subprocess.run(cmd,cwd=work,stdout=log,stderr=subprocess.STDOUT,timeout=240)
  text=(out/(label+'.log')).read_text();assert not r.returncode,(label,r.returncode,text[-2000:]);return text
 try:
  for ss in (512,4096):
   tag=f'4096-{ss}';setup=base/(tag+'-setup.img');en=base/(tag+'-enrollment.bin');pol=base/(tag+'-policy.bin')
   def probe(mode,image,pre):return run([str(base/'probe'),mode,str(image),str(ss),str(en),str(pol),str(pre)],pre.name)
   for seed,path in [(9,'/var/lib/fortress/accounts/intent.tmp'),(18,'/etc/group.publish'),(24,'/etc/shadow.publish')]:
    pre=out/(tag+f'-seed{seed}');original_report=probe(f'update={seed}',setup,pre);image=pathlib.Path(str(pre)+'-interrupted.img')
    stat=run(['debugfs','-R','stat '+path,str(image)],pre.name+'-stat');assert 'Inode:' in stat and ('Size: 160' in stat if seed==9 else 'Size: 8192' in stat)
    fresh=out/(pre.name+'-baseline');text=probe('recover',image,fresh);commits=int(re.search(r'PASS recovery commits=(\d+)',text)[1]);pathlib.Path(str(fresh)+'-final.img').unlink()
    def cut_case(cut):
     pre=out/(tag+f'-seed{seed}-cut{cut}');interrupted=probe(f'recover={cut}',image,pre);current=pathlib.Path(str(pre)+'-interrupted.img');h=sha(current);reports=[]
     for n in range(2):
      new=out/(pre.name+f'-restart{n}');reports.append(probe('recover',current,new));final=pathlib.Path(str(new)+'-final.img');run(['e2fsck','-fn',str(final)],new.name+'-fsck')
      if n:assert 'commits=0' in reports[-1]
      current.unlink();current=final
     current.unlink();return {'cut':cut,'interrupt':interrupted,'reports':reports,'interrupted_sha256':h}
    with ThreadPoolExecutor(max_workers=8) as pool:cuts=list(pool.map(cut_case,range(1,commits+1)))
    result['cases'].append({'sector':ss,'seed':seed,'path':path,'seed_sha256':sha(image),'commits':commits,'cuts':cuts});print(tag,seed,commits,'PASS',flush=True)
  result['status']='PASS'
 except Exception as e:result.update(status='FAIL',error=repr(e));raise
 finally:
  result['base_preserved']=sha(base/'probe')==result['probe_sha256'] and sha(base/'manifest.json')==result['base_manifest_sha256'] and all(sha(work/p)==h for p,h in m['compiled_inputs'].items())
  if not result['base_preserved']:result['status']='FAIL'
  result['artifacts']={p.name:sha(p) for p in out.iterdir() if p.is_file()};(out/'manifest.json').write_text(json.dumps(result,indent=2)+'\n');print('Evidence:',out,result['status'],flush=True)
 assert result['status']=='PASS'
if __name__=='__main__':main()
