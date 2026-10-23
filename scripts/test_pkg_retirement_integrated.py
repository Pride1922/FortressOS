"""One bounded private retirement integration/acceptance campaign. RAM disks only."""
import argparse,datetime,hashlib,json,os,pathlib,re,shutil,struct,subprocess,textwrap,tempfile,time
from concurrent.futures import ThreadPoolExecutor
from test_ext4_time_host import fixture
from test_pkg_enrollment import signed,policy
from test_pkg_service_integration import function
from pkg_account_service_overlay import apply
from pkg_mount_overlay import apply_mount
from pkg_retirement_overlay import apply_retirement
from pkg_mount_vectors import vectors,NOW
from test_pkg_retirement_auth import sign,PUB,anchor
ROOT=pathlib.Path(__file__).resolve().parents[1]
H=lambda b:hashlib.sha256(b).digest()
def put(b,o,v,f='Q'):struct.pack_into('<'+f,b,o,v)
def seal(b):b[-32:]=H(b[:-32])
def main():
 parser=argparse.ArgumentParser();parser.add_argument('--block',type=int,choices=(1024,2048,4096));parser.add_argument('--sector',type=int,choices=(512,4096));parser.add_argument('--smoke',action='store_true');parser.add_argument('--exhaustive',action='store_true');parser.add_argument('--jobs',type=int,default=4);args=parser.parse_args()
 assert not args.exhaustive or (args.block and args.sector),'name the exhaustive geometry explicitly'
 out=ROOT/'.codex-remote-attachments'/('pkg-retirement-integrated-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'));work=out/'workspace';work.mkdir(parents=True)
 sha=lambda p:H(p.read_bytes()).hex();sources={}
 for folder in ('src','tests','user','scripts'):
  for p in (ROOT/folder).rglob('*'):
   if p.is_file() and p.suffix in ('.c','.h','.inc','.py','.mk') and '__pycache__' not in p.parts:
    name=str(p.relative_to(ROOT));sources[name]=sha(p);dst=work/name;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,dst)
 guards={str(p.relative_to(ROOT)):sha(p) for p in [ROOT/'AGENTS.md',ROOT/'README.md',ROOT/'assets/ssl/ca-certificates.sha256',*(p for p in (ROOT/'bin').iterdir() if p.name in ('fortress.elf','initramfs.tar','fortress.iso','fortress.img'))]}
 manifest=dict(status='INCOMPLETE',sources=sources,guards=guards,overlay=[],commands=[],cases=[],scope='one private durable-evidence, mount-preview, actual account-service and JBD2 retirement milestone; no production, installation, boot or physical acceptance')
 (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
 def replace(name,before,after):
  p=work/name;text=p.read_text();assert text.count(before)==1,(name,before,text.count(before));p.write_text(text.replace(before,after));manifest['overlay'].append(dict(path=name,before=before,after=after))
 # Reuse only the existing exact overlay construction; no old main/test campaign.
 source=(work/'scripts/test_pkg_mount_admission.py').read_text()
 start=source.index("    replace('src/fs/ext4_transaction.inc'");end=source.index('    env=dict(os.environ)',start)
 context=dict(work=work,replace=replace,function=function,apply=apply,apply_mount=apply_mount)
 exec(compile(textwrap.dedent(source[start:end]),'private-existing-mount-overlay','exec'),context)
 generated=context['generated'];apply_retirement(work,replace,generated)
 env=dict(os.environ);inputs=out/'fixture-inputs';extra=vectors(inputs);env.update(PKG_MOUNT_INPUT=str(inputs),PKG_MOUNT_PHASE='3',PKG_MOUNT_PUBLICATION='2')
 def run(cmd,label,timeout=180):
  started=time.monotonic();manifest['commands'].append(cmd);r=subprocess.run(cmd,cwd=work,text=True,capture_output=True,timeout=timeout,env=env)
  (out/(label+'.log')).write_text(r.stdout+r.stderr);assert r.returncode==0,(label,r.returncode,r.stdout[-1400:],r.stderr[-2400:]);manifest.setdefault('timings',[]).append(dict(label=label,seconds=time.monotonic()-started));return r.stdout+r.stderr
 try:
  flags=['gcc','-std=c11','-O1','-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-no-pie','-pthread','-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
  flags+=['-I'+p for p in ('tests','tests/ext4_host','tests/host','src/include','src/fs','src/drivers','src/kernel','src/arch/x86_64','src/mm','user/tools','src/lib/crypto/upstream/src','.')]
  objects=[]
  for tag,path in (('mono','src/lib/crypto/upstream/src/monocypher.c'),('ed','src/lib/crypto/upstream/src/optional/monocypher-ed25519.c')):
   obj=str(out/(tag+'.o'));run(flags+['-include','src/lib/crypto/namespace.h','-c',path,'-o',obj],'compile-'+tag);objects.append(obj)
  units=['tests/account-service-durable.c','tests/pkg_enrollment_candidate.c','tests/ext4_fault_disk.c','src/fs/permission_values.c','src/kernel/creds.c','user/tools/digest.c','src/lib/crypto/hash.c','src/lib/crypto/curve.c','src/lib/account_redo.c','user/tools/userdb.c','tests/pkg_mount_candidate.c','tests/pkg_grant_candidate.c','tests/pkg_retirement_candidate.c','tests/pkg_retirement_auth.c','user/pkg/catalog.c','user/pkg/verify.c','user/pkg/format.c','user/pkg/manifest.c','user/tools/gzip.c']
  dep=run(flags+['-MM','-MT','probe',*units],'dependencies')
  names={p for p in dep.replace('\\\n',' ').split() if not p.endswith(':')};names.update(('src/lib/crypto/upstream/src/monocypher.c','src/lib/crypto/upstream/src/optional/monocypher-ed25519.c','src/lib/crypto/namespace.h'))
  manifest['compiled_inputs']={p:sha(work/p) for p in names}
  run(flags+units+objects+['-o',str(out/'probe')],'compile')
  for bs in ((args.block,) if args.block else (1024,2048,4096)):
   image=fixture(out,bs,inodes=128,size_mib=32 if bs==4096 else 16);original=sha(image)
   for ss in ((args.sector,) if args.sector else (512,4096)):
    tag=f'{bs}-{ss}';pre=out/tag
    run([str(out/'probe'),'setup',str(image),str(ss),str(pre)],tag+'-setup')
    setup=pathlib.Path(str(pre)+'-setup.img');raw=setup.read_bytes();body=pathlib.Path(str(pre)+'-body.img').read_bytes();loc=pathlib.Path(str(pre)+'-locations.img').read_bytes()
    def read_record(i):
     r=body[416+128*i:544+128*i];n=struct.unpack_from('<Q',r,16)[0];parts=[]
     for j in range(struct.unpack_from('<H',r,32)[0]):
      logical,count,physical=struct.unpack_from('<IIQ',r,40+16*j);parts.append(raw[physical*bs:(physical+count)*bs])
     return bytearray(b''.join(parts)[:n])
    m,g,t,o,a=(read_record(i) for i in (0,1,2,4,5));put(a,20,4,'H')
    desc=signed(body);en=out/(tag+'-enrollment.bin');en.write_bytes(desc)
    art=(inputs/'artifact.fpk').read_bytes();exe=(inputs/'executable.bin').read_bytes();tr=(inputs/'trust.bin').read_bytes();state=(inputs/'next.bin').read_bytes();intent=(inputs/'pending.bin').read_bytes()
    c=bytearray(1024);c[:16]=b'FORT-PKG-RET-v1\0';put(c,20,1,'H');put(c,24,struct.unpack_from('<Q',body,24)[0]);c[32:112]=body[40:120];c[112:144]=H(desc);c[144:176]=H(m);c[176:208]=body[152:184];c[208:240]=a[80:112];c[240:256]=g[40:56]
    for off,b in ((256,tr),(288,state),(320,intent),(352,art),(384,exe)):c[off:off+32]=H(b)
    for off,v in ((416,len(art)),(424,len(exe)),(432,NOW-1),(440,NOW+60),(448,NOW),(456,2),(496,struct.unpack_from('<Q',g,24)[0]),(504,struct.unpack_from('<Q',a,24)[0]),(512,struct.unpack_from('<Q',a,144)[0])):put(c,off,v)
    c[464:496]=extra[336:368];c[520:584]=t[256:320];c[584:616]=H(g);c[616:648]=H(t);c[648:680]=anchor(a);c[680:712]=anchor(o,False);c=sign(c)
    cert=out/(tag+'-certificate.bin');cert.write_bytes(c)
    l=bytearray(1024);l[:16]=b'FORT-PKG-RLG-v1\0';put(l,16,1024,'I');put(l,20,1,'H');l[24:40]=g[24:40];l[40:104]=g[40:104];l[104:136]=H(c);l[136:168]=H(g);l[168:200]=H(t);l[200:208]=a[24:32];l[208:216]=a[144:152]
    l[216:220]=t[256:260];l[220:224]=t[268:272];l[224:232]=t[260:268];l[232:240]=t[288:296];l[240:288]=g[296:344];l[288:304]=t[296:312];seal(l);a[592:624]=H(l);seal(a)
    lp=out/(tag+'-ledger.bin');lp.write_bytes(l);ap=out/(tag+'-account.bin');ap.write_bytes(a)
    pol=out/(tag+'-policy.bin');pol.write_bytes(policy(desc,uuids=[body[40+16*i:56+16*i] for i in range(5)])+extra+b''.join(PUB[:3]).ljust(256,b'\0')+struct.pack('<II',3,2)+H(c)+loc)
    def probe(mode,img,label,rules=pol,descriptor=en,destination=None):
     cmd=[str(out/'probe'),mode,str(img),str(ss),str(descriptor),str(rules),str(out/label if destination is None else destination)]
     if mode.startswith('activate'):cmd += [str(lp),str(ap),str(cert)]
     return run(cmd,label,timeout=300)
    activation=tag+'-activation';text=probe('activate',setup,activation);activation_events=int(re.search(r'PASS activation result=0 events=(\d+)',text)[1]);active=out/(activation+'-final.img')
    baseline={}
    for mode,img in (('unlink',active),('close',active)):
     label=tag+'-'+mode;text=probe(mode,img,label);baseline[mode]=int(re.search(r'PASS '+mode+r'(?: result=0)? events=(\d+)',text)[1])
    orphan=out/(tag+'-unlink-final.img');text=probe('recover',orphan,tag+'-recover');baseline['recover']=int(re.search(r'PASS recovery events=(\d+)',text)[1])
    final=out/(tag+'-recover-final.img');again=probe('recover',final,tag+'-again');assert 'commits=0' in again
    case=dict(block=bs,sector=ss,status='PASS',events=baseline,interruptions=[],refusals=[])
    # Independent native-filesystem and all-record oracle on retained outputs.
    positions={name:struct.unpack_from('<Q',body,416+128*i+48)[0]*bs for name,i in (('marker',0),('grant',1),('owners',2),('origin',4),('account',5),('state',6),('trust',7))}
    positions.update({name:struct.unpack_from('<Q',loc,128*i+48)[0]*bs for i,name in enumerate(('ledger','certificate','enrollment','evidence'))})
    initial_free=struct.unpack_from('<II',active.read_bytes(),1036)
    def audit(path,label,retired):
     before=sha(path);run(['e2fsck','-fn',str(path)],label+'-fsck');dumped={}
     for key,name in (('grant','grant.v1'),('owners','ownership.v1'),('ledger','retirement.v1'),('account','account-generation.v1'),('origin','origin.v1'),('certificate','retirement.cert'),('enrollment','enrollment.cert'),('evidence','evidence.fpk')):
      dst=out/(label+'-'+name+'.dump');run(['debugfs','-R',f'dump /pkg/{name} {dst}',str(path)],label+'-'+name);dumped[key]=dst.read_bytes()
     gg,tt,ll,aa,oo=(dumped[k] for k in ('grant','owners','ledger','account','origin'))
     for b in (gg,tt,ll,aa,oo):assert H(b[:-32])==b[-32:]
     assert H(tt)==gg[264:296] and H(gg)==ll[136:168] and H(tt)==ll[168:200]
     assert H(oo)==aa[112:144] and H(gg)==aa[448:480] and H(ll)==aa[592:624] and H(c)==ll[104:136]
     assert gg[24:32]==tt[48:56]==ll[24:32]==aa[440:448] and aa[24:32]==oo[24:32]==ll[200:208]
     assert dumped['certificate']==c and dumped['enrollment']==desc and dumped['evidence']==art
     q=[0,0]
     for i in range(4):
      row=tt[256+64*i:320+64*i];role,cls,ino,gen,flags=struct.unpack_from('<HHIII',row);n,blocks,inodes=struct.unpack_from('<QQQ',row,32)
      stat=run(['debugfs','-R',f'stat <{ino}>',str(path)],label+f'-role{role}')
      assert int(re.search(r'Generation:\s+(\d+)',stat)[1])==gen
      if cls==3:
       assert i==0 and retired and not (flags or n or blocks or inodes)
       bitmap=run(['debugfs','-R',f'testi <{ino}>',str(path)],label+'-freed-inode');assert 'not in use' in bitmap
      else:
       assert cls==1 and flags==1 and inodes==1 and re.search(r'User:\s+0\s+Group:\s+0',stat)
       assert int(re.search(r'Links:\s+(\d+)',stat)[1])==1 and int(re.search(r'Size:\s+(\d+)',stat)[1])==n
       assert int(re.search(r'Mode:\s+([0-7]+)',stat)[1],8)==(0o755 if role==17 else 0o600)
       actual=int(re.search(r'Blockcount:\s+(\d+)',stat)[1]);assert actual*512==blocks*bs;q[0]+=blocks;q[1]+=1
     assert tuple(q)==struct.unpack_from('<QQ',gg,328)
     assert struct.unpack_from('<QQ',gg,312)==((13,5) if retired else (12,4))
     assert struct.unpack_from('<II',path.read_bytes(),1036)==tuple(v+int(retired) for v in initial_free)
     assert sha(path)==before
    for name,path,retired in (('activated',active,False),('closed',out/(tag+'-close-final.img'),True),('recovered',final,True),('again',out/(tag+'-again-final.img'),True)):audit(path,tag+'-'+name,retired)
    raw_active=active.read_bytes();raw_orphan=orphan.read_bytes()
    def reject(label,data,rules=None,descriptor=None):
     target=out/(tag+'-reject-'+label+'.img');target.write_bytes(data);before=sha(target)
     report=probe('reject',target,tag+'-reject-'+label,pol if rules is None else rules,en if descriptor is None else descriptor)
     assert 'writes=0 flushes=0' in report and sha(target)==before;target.unlink();case['refusals'].append(label)
    reject('required-uninitialized',setup.read_bytes())
    for label,at in (('catalog-root',400),('catalog-floor',408),('catalog-hash',424),('catalog-key',464),('base',752),('platform',720),('retirement-key',760),('certificate-pin',1024),('ledger-locator-generation',1064),('enrollment-key',0)):
     rules=bytearray(pol.read_bytes());rules[at]^=1;rp=out/(tag+'-'+label+'.policy');rp.write_bytes(rules);reject(label,raw_active,rp)
    rules=bytearray(pol.read_bytes());put(rules,416,0);rp=out/(tag+'-clock.policy');rp.write_bytes(rules);reject('invalid-clock',raw_active,rp)
    for key in ('certificate','enrollment','evidence','state','trust','ledger'):
     data=bytearray(raw_active);data[positions[key]]^=1;reject('damaged-'+key,data)
    # Checksummed metadata must not choose the authority or physical victim.
    la,aa=positions['ledger'],positions['account'];ca=positions['certificate']
    forged=bytearray(c);forged[-1]^=1;data=bytearray(raw_active);data[ca:ca+len(c)]=forged
    ll=bytearray(data[la:la+1024]);ll[104:136]=H(forged);seal(ll);data[la:la+1024]=ll
    account=bytearray(data[aa:aa+1024]);account[592:624]=H(ll);seal(account);data[aa:aa+1024]=account
    rules=bytearray(pol.read_bytes());rules[1024:1056]=H(forged);rp=out/(tag+'-coherent-forged-certificate.policy');rp.write_bytes(rules);reject('coherent-forged-certificate',data,rp)
    data=bytearray(raw_active);account=bytearray(data[aa:aa+1024]);put(account,22,1,'H');seal(account);data[aa:aa+1024]=account;reject('active-account-snapshot',data)
    for mask in range(31):
     data=bytearray(raw_orphan)
     for bit,(key,n) in enumerate((('grant',1024),('owners',8192),('ledger',1024),('account',1024),('origin',1024))):
      if not mask&(1<<bit):at=positions[key];data[at:at+n]=raw_active[at:at+n]
     reject('mixed-five-'+str(mask),data)
    # Allocation and native-content checks that the pure certificate cannot do.
    for label,path in (('native-archive','/pkg/transaction.v1/artifact.fpk'),('native-next','/pkg/transaction.v1/next.v1'),('native-passwd','/etc/passwd')):
     text=run(['debugfs','-R','blocks '+path,str(active)],tag+'-'+label+'-map');physical=int(re.search(r'(?m)^([0-9]+)',text)[1])
     data=bytearray(raw_active);data[physical*bs]^=1;reject(label,data)
    rules=bytearray(pol.read_bytes());put(rules,416,NOW+3600);expired=out/(tag+'-expired.policy');expired.write_bytes(rules)
    report=probe('recover',active,tag+'-expired',expired);assert 'PASS recovery' in report
    (out/(tag+'-expired-final.img')).unlink()
    # Exact account capacity behind a larger, correctly re-certified escrow.
    # This is an initialized grant fixture, not a production grant issuer.
    pressure=bytearray(raw_active);gg=bytearray(pressure[positions['grant']:positions['grant']+1024])
    for i,reserve in enumerate((9*((8192+bs-1)//bs+int((8192+bs-1)//bs>4))+2-3,8)):
     free=struct.unpack_from('<I',pressure,1036+4*i)[0];q=struct.unpack_from('<Q',gg,328+8*i)[0];remaining=free-reserve
     put(gg,312+8*i,remaining);put(gg,296+8*i,remaining+q)
    seal(gg);pressure[positions['grant']:positions['grant']+1024]=gg
    cc=bytearray(c);cc[584:616]=H(gg);cc=sign(cc)
    ll=bytearray(pressure[la:la+1024]);ll[104:136]=H(cc);ll[136:168]=H(gg);ll[240:288]=gg[296:344];seal(ll);pressure[la:la+1024]=ll
    account=bytearray(pressure[aa:aa+1024]);account[448:480]=H(gg);account[592:624]=H(ll);seal(account);pressure[aa:aa+1024]=account
    pressure[ca:ca+len(cc)]=cc;rules=bytearray(pol.read_bytes());rules[1024:1056]=H(cc)
    pressure_image=out/(tag+'-pressure.img');pressure_image.write_bytes(pressure);pressure_policy=out/(tag+'-pressure.policy');pressure_policy.write_bytes(rules)
    report=probe('unlink',pressure_image,tag+'-pressure-unlink',pressure_policy);assert 'PASS exact reserve competing block/inode allocations zero-io refused' in report
    pressure_orphan=out/(tag+'-pressure-unlink-final.img');report=probe('recover',pressure_orphan,tag+'-pressure-recover',pressure_policy);assert 'PASS exact reserve competing block/inode allocations zero-io refused' in report
    pressure_final=out/(tag+'-pressure-recover-final.img');run(['e2fsck','-fn',str(pressure_final)],tag+'-pressure-fsck')
    case['pressure']='PASS';case['pressure_reserve']=(9*((8192+bs-1)//bs+int((8192+bs-1)//bs>4))+2-3,8)
    # A real committed/uncheckpointed activation is checked before replay.
    pause=tag+'-activation-pause';probe('activate-pause',setup,pause);pending=out/(pause+'-interrupted.img')
    wrong=bytearray(pol.read_bytes());wrong[1024]^=1;rp=out/(tag+'-pending-wrong.policy');rp.write_bytes(wrong)
    reject('committed-journal-wrong-policy',pending.read_bytes(),rp)
    report=probe('recover',pending,tag+'-pending-replay');assert 'XR preview-newer=1' in report
    audit(out/(tag+'-pending-replay-final.img'),tag+'-pending-replay',False)
    case['linux_audits']=6;case['activation_events']=activation_events
    manifest['cases'].append(case);print(tag,'functional/adversarial integration PASS',flush=True)
    if not args.smoke:
     tasks=[(mode,profile,cut) for mode,total in dict(baseline,activate=activation_events).items() for profile in range(8) for cut in (range(total) if args.exhaustive else sorted({0,total//2,total-1}))]
     # Large transient images stay on the Linux filesystem. Successful images
     # are discarded only after two fresh-process checks; failures are retained.
     scratch=pathlib.Path(tempfile.mkdtemp(prefix='fortress-retirement-'))
     assert scratch.is_absolute() and str(scratch).startswith('/tmp/fortress-retirement-')
     source_guards={p:sha(p) for p in (active,orphan,setup,en,pol,cert,lp,ap)}
     def interrupt(task):
      mode,profile,cut=task;label=tag+f'-{mode}-io-{profile}-{cut}';source=setup if mode=='activate' else orphan if mode=='recover' else active
      paths=[]
      try:
       prefix=scratch/label
       probe(f'{mode}={profile}:{cut}',source,label,destination=prefix)
       broken=pathlib.Path(str(prefix)+'-interrupted.img');paths.append(broken);assert broken.exists()
       account_seq=None;admitted=None;digests=[]
       for restart in range(2):
        dest=label+f'-restart{restart}';prefix=scratch/dest
        result=probe('inspect' if mode=='activate' else 'recover',broken,dest,destination=prefix)
        new=pathlib.Path(str(prefix)+'-final.img');paths.append(new);digests.append(sha(new))
        is_admitted='PASS activation-refused' not in result
        if restart:assert admitted==is_admitted
        admitted=is_admitted
        if not is_admitted:
         assert mode=='activate' and sha(new)==sha(broken)
        else:
         observed=int(re.search(r'XR seq=\d+ account=(\d+)',result)[1])
         if restart:assert observed==account_seq and 'commits=0' in result
         account_seq=observed
        assert broken.resolve().parent==scratch.resolve();broken.unlink();broken=new
       broken.unlink()
       return dict(mode=mode,profile=profile,cut=cut,restarts=2,admitted=admitted,account_sequence=account_seq,output_sha256=digests)
      except Exception:
       retained=out/'retained-failures'/label;retained.mkdir(parents=True,exist_ok=True)
       for path in paths:
        if path.exists():shutil.copyfile(path,retained/path.name)
       raise
     try:
      with ThreadPoolExecutor(max_workers=args.jobs) as pool:case['interruptions']=list(pool.map(interrupt,tasks))
     finally:
      assert scratch.resolve().parent==pathlib.Path('/tmp') and scratch.name.startswith('fortress-retirement-')
      shutil.rmtree(scratch)
     assert all(sha(p)==h for p,h in source_guards.items())
    assert sha(image)==original
  manifest['status']='PASS'
 except Exception as e:manifest['status']='FAIL';manifest['error']=repr(e);raise
 finally:
  manifest['changed_compiled_copies']=[p for p,h in manifest.get('compiled_inputs',{}).items() if sha(work/p)!=h]
  manifest['changed_repository_sources']=[p for p,h in sources.items() if sha(ROOT/p)!=h]
  manifest['changed_guards']=[p for p,h in guards.items() if sha(ROOT/p)!=h]
  if manifest['changed_compiled_copies'] or manifest['changed_repository_sources'] or manifest['changed_guards']:manifest['status']='FAIL'
  (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');print('Evidence:',out,manifest['status'],flush=True)
 assert manifest['status']=='PASS'
if __name__=='__main__':main()
