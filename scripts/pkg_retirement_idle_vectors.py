"""Independent signing oracle for one bounded completion and fresh-writer profile."""
import copy,hashlib,struct
from test_pkg_retirement_auth import KEYS,PUB
H=lambda b:hashlib.sha256(b).digest()
def put(b,o,v,f='Q'):struct.pack_into('<'+f,b,o,v)
def seal(b):b[-32:]=H(b[:-32])
def refresh(b,idle=False):
 g,t,l,a,o=b
 t[-32:]=H(t[:-32]);g[264:296]=H(t);seal(g)
 if idle:l[:]=bytes(1024)
 else:
  l[24:32]=g[24:32];l[136:168]=H(g);l[168:200]=H(t);l[200:208]=a[24:32];l[218:224]=t[258:260]+t[268:272];l[232:240]=t[288:296];l[240:288]=g[296:344];l[288:304]=t[296:312];seal(l)
 o[24:32]=a[24:32];o[40:48]=a[32:40];seal(o);a[112:144]=H(o);a[440:448]=g[24:32];a[448:480]=H(g);a[480:512]=g[232:264];a[512:544]=g[200:232];a[544:576]=g[136:168];a[576:592]=g[40:56];a[592:624]=bytes(32) if idle else H(l);seal(a)
def advance(b,stage):
 b=copy.deepcopy(b);g,t,l,a,o=b
 if stage in (12,13):
  put(a,24,struct.unpack_from('<Q',a,24)[0]+1);put(a,22,int(stage==12),'H')
  if stage==12:put(a,32,struct.unpack_from('<Q',a,32)[0]+1)
  # Native account BEGIN/END updates only the paired records.
  o[24:32]=a[24:32];o[40:48]=a[32:40];seal(o);a[112:144]=H(o);seal(a);return b
 for x in (g,a):put(x,24,struct.unpack_from('<Q',x,24)[0]+1)
 t[48:56]=g[24:32]
 if stage==11:
  put(g,22,0,'H');g[40:56]=bytes(16);g[136:168]=bytes(32);g[168:200]=g[200:232];g[296:344]=bytes(48);put(g,348,0,'H');g[360:368]=g[368:376];g[392:432]=bytes(40)
  put(t,22,0,'H');t[32:48]=bytes(16);t[256:8160]=bytes(7904);refresh(b,True);return b
 row=256+64*(0 if stage<=3 else 1 if stage==4 else 2 if stage<=7 else 3)
 if stage in (1,5,8):put(t,row+12,2,'I')
 elif stage==4:
  for d in range(2):
   amount=struct.unpack_from('<Q',t,row+40+8*d)[0]
   for off in (296,328):put(g,off+8*d,struct.unpack_from('<Q',g,off+8*d)[0]-amount)
  put(t,row+2,3,'H');t[row+12:row+16]=bytes(4);t[row+32:row+56]=bytes(24)
 else:
  dimension=0 if stage in (2,6,9) else 1
  amount=struct.unpack_from('<Q',t,row+40+8*dimension)[0];put(t,row+40+8*dimension,0)
  put(g,312+8*dimension,struct.unpack_from('<Q',g,312+8*dimension)[0]+amount);put(g,328+8*dimension,struct.unpack_from('<Q',g,328+8*dimension)[0]-amount)
  if dimension:put(t,row+2,3,'H');t[row+12:row+16]=bytes(4);t[row+32:row+40]=bytes(8)
 refresh(b);return b

def completion(out,tag,raw,body,m,g,t,l,a,o,c,en,state,tr,art,exe):
 states=[[bytearray(x) for x in (g,t,l,a,o)]]
 for stage in range(1,14):states.append(advance(states[-1],stage))
 cert=bytearray(1536);cert[:16]=b'FORT-PKG-END-v1\0';put(cert,16,1728,'I');put(cert,20,1,'H');put(cert,22,14,'H');cert[24:112]=body[24:32]+body[40:120]
 for off,x in ((112,c),(144,en),(176,state),(208,tr),(240,art),(272,exe)):cert[off:off+32]=H(x)
 for i,b in enumerate(states):cert[304+32*i:336+32*i]=H(b''.join(b))
 cert[768:1024]=t[256:512];cert[1024:1032]=raw[1036:1040]+bytes(4);cert[1032:1040]=raw[1040:1044]+bytes(4)
 cert[1040:1048]=a[32:40]
 cert+=b''.join(i+s for i,s in sorted((H(PUB[k]),KEYS[k].sign(bytes(cert))) for k in (0,1)))
 (out/(tag+'-states.bin')).write_bytes(b''.join(b''.join(b) for b in states))
 return cert,states
