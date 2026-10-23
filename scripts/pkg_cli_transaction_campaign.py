"""Private CLI/real-engine acceptance; inputs remain immutable, failures retained."""
import pathlib,re,hashlib,tempfile,shutil,struct
from concurrent.futures import ThreadPoolExecutor
H=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
def campaign(out,tag,bs,ss,active,pol,en,probe,run,case_manifest,args,states,raw,body):
 case=dict(block=bs,sector=ss,status='INCOMPLETE',interruptions=[]);case_manifest['cases'].append(case)
 guards={p:H(p) for p in (active,pol,en)}
 def call(mode,image,label,**kwargs):return probe(mode,image,tag+'-'+label,**kwargs)
 text=call('status',active,'status-active');assert 'state: RETIRING' in text and 'remaining blocks: 12 inodes: 4' in text
 call('syntax',active,'syntax');call('invalid-result',active,'invalid-result')
 text=call('recover',active,'recover');assert 'recovery complete: IDLE' in text and 'commits=11' in text
 events=int(re.search(r'PASS CLI recover events=(\d+)',text)[1]);idle=out/(tag+'-recover-final.img')
 text=call('status',idle,'status-idle');assert 'state: IDLE' in text and 'remaining blocks: 0 inodes: 0' in text and 'charged blocks: 0 inodes: 0' in text
 text=call('recover',idle,'again');assert 'commits=0' in text
 text=call('output-fail',active,'output-fail');assert 'commits=11' in text and 'state: IDLE' in text and 'recovery complete:' not in text
 # The existing private engine supplies a real committed, uncheckpointed IDLE image.
 call('terminal-pause',active,'pause');pending=out/(tag+'-pause-interrupted.img');text=call('status',pending,'status-pending-journal');assert 'state: IDLE' in text and 'writes=0 flushes=0' in text
 call('recover',pending,'journal-replay')
 locs=[body[416+128*i:544+128*i] for i in (1,2)]+[(out/(tag+'-locations.img')).read_bytes()[:128]]+[body[416+128*i:544+128*i] for i in (5,4)]
 def records(path):
  image=path.read_bytes();result=[]
  for loc in locs:
   n=struct.unpack_from('<Q',loc,16)[0];data=b''
   for i in range(struct.unpack_from('<H',loc,32)[0]):
    logical,count,physical=struct.unpack_from('<IIQ',loc,40+16*i);data+=image[physical*bs:(physical+count)*bs]
   result.append(data[:n])
  return result
 expected=[bytes(x) for x in states[11]]
 for label in ('recover','again','output-fail','journal-replay'):
  image=out/(tag+'-'+label+'-final.img');assert records(image)==expected;run(['e2fsck','-fn',str(image)],tag+'-'+label+'-fsck')
 # Bad durable evidence must refuse both status and recovery without any I/O.
 damaged=bytearray(active.read_bytes());loc=(out/(tag+'-completion-locator.img')).read_bytes();physical=struct.unpack_from('<Q',loc,48)[0];damaged[physical*bs+1536+32]^=1;bad=out/(tag+'-bad.img');bad.write_bytes(damaged)
 text=call('status-refuse',bad,'bad-status');assert 'state:' not in text
 text=call('refuse',bad,'bad-recover');assert 'recovery complete:' not in text and H(bad)==hashlib.sha256(damaged).hexdigest()
 case.update(status='PASS',events=events,linux_audits=4,cli_scenarios=11)
 print(tag,'CLI status/recover/output-failure and refusal PASS',flush=True)
 if args.smoke:return
 scratch=pathlib.Path(tempfile.mkdtemp(prefix='fortress-cli-'));assert scratch.parent==pathlib.Path('/tmp');source=scratch/'source.img';shutil.copyfile(active,source);assert H(source)==H(active)
 def interrupt(task):
  profile,cut=task;label=tag+f'-io-{profile}-{cut}';paths=[]
  try:
   prefix=scratch/label;text=probe(f'recover={profile}:{cut}',source,label,destination=prefix);assert 'PASS CLI interrupted' in text and 'recovery complete:' not in text
   broken=pathlib.Path(str(prefix)+'-interrupted.img');paths.append(broken)
   for restart in range(2):
    prefix=scratch/(label+f'-restart{restart}');text=probe('recover',broken,label+f'-restart{restart}',destination=prefix);new=pathlib.Path(str(prefix)+'-final.img');paths.append(new);assert records(new)==expected
    if restart:assert 'commits=0' in text
    broken.unlink();broken=new
   broken.unlink();return dict(profile=profile,cut=cut,restarts=2)
  except Exception:
   dest=out/'retained-failures'/label;dest.mkdir(parents=True,exist_ok=True)
   for p in paths:
    if p.exists():shutil.copyfile(p,dest/p.name)
   raise
 try:
  tasks=[(p,c) for p in range(8) for c in (range(events) if args.exhaustive else sorted({0,events//2,events-1}))]
  with ThreadPoolExecutor(max_workers=args.jobs) as pool:case['interruptions']=list(pool.map(interrupt,tasks))
 finally:
  assert scratch.parent==pathlib.Path('/tmp') and scratch.name.startswith('fortress-cli-') and H(source)==H(active);shutil.rmtree(scratch)
 assert all(H(p)==h for p,h in guards.items())
