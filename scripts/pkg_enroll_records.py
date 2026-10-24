"""Initial IDLE records for fresh disposable TEST-only enrollment fixtures.

No device interface or existing-root enrollment. Signing keys must be external
private files. Geometry and maps are observations until independently checked.
"""
import hashlib, json, os, pathlib, struct, uuid
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding,PublicFormat
from pkg_enroll_fixture import FS,PART,FILES,BS
from pkg_enroll_identity import PATHS,inventory
from pkg_test_enroll import NAMES,ROOT,regular,prepare
H=lambda b:hashlib.sha256(b).digest()
PUB=lambda k:k.public_key().public_bytes(Encoding.Raw,PublicFormat.Raw)

def keys(paths):
    result=[]
    for p in paths:
        p=pathlib.Path(p).resolve()
        if p.is_relative_to(ROOT) or p.stat().st_mode&0o077:raise ValueError('external private TEST seeds required')
        raw=regular(p,32)
        if len(raw)!=32:raise ValueError('32-byte TEST seed required')
        k=Ed25519PrivateKey.from_private_bytes(raw)
        if any(PUB(k)==PUB(other) for other in result):raise ValueError('duplicate TEST key')
        result.append(k)
    if len(result)!=3:raise ValueError('exact three-key TEST role required')
    return result

def sealed(b):
    b[-32:]=H(b[:-32]);return bytes(b)

def header(domain,size=1024,version=1):
    b=bytearray(size);b[:16]=domain.encode().ljust(16,b'\0')
    struct.pack_into('<IHH',b,16,size,version,0);return b

def signed(body,rolekeys):
    return bytes(body)+b''.join(i+s for i,s in sorted((H(PUB(k)),k.sign(bytes(body))) for k in rolekeys[:2]))

def build(image,out,seed_roles,kernel,bootstrap,now=1792843200,artifact=None,root_version=3,catalog_revision=7):
    """Prepare records only. Does not publish selector or mutate image."""
    out=pathlib.Path(out);out.mkdir(mode=0o700)
    if len(kernel)!=32 or len(bootstrap)!=32 or not any(kernel) or not any(bootstrap):raise ValueError('measurement hashes required')
    if not 1<=root_version<2**64-1 or not 1<=catalog_revision<2**64-1:raise ValueError('bounded trust versions required')
    rolekeys=[keys(seed_roles[role]) for role in ('selector','enrollment','catalog')]
    if len({PUB(k) for group in rolekeys for k in group})!=9:raise ValueError('disjoint TEST roles required')
    witness,audit=inventory(image);rows=[witness[32+i*128:32+(i+1)*128] for i in range(20)]
    ids=[FS.bytes,PART.bytes,uuid.UUID('62345678-1234-4567-89ab-123456789abc').bytes,
         bytes([4])*16,uuid.UUID('72345678-1234-4567-89ab-123456789abc').bytes]
    epoch=5;profile=H(b'Fortress bounded mount profile v1');account_profile=H(b'Fortress TEST root-only accounts v1')
    platform=bytes(range(32));base=2
    root={'profile':'fortress-pkg2-test-only-v1','version':root_version,'repository':ids[3].hex(),
        'roles':{name:{'threshold':2,'keys':[PUB(k).hex() for k in group]} for name,group in zip(('selector','enrollment','catalog'),rolekeys)}}
    root_bytes=json.dumps(root,sort_keys=True,separators=(',',':')).encode()
    # Catalog authorizes a future demo artifact; the genesis inventory is empty.
    if artifact is not None and not 128<=len(artifact)<=8192:raise ValueError('bounded TEST artifact required')
    entry=bytes([4])+b'demo'.ljust(32,b'\0')+struct.pack('<QQ',1,len(artifact) if artifact is not None else 128)+(H(artifact) if artifact is not None else H(b'TEST future demo artifact'))
    catalog=signed(struct.pack('<16s16sQQQQHHI',b'FORT-PKG-CAT-v1\0',ids[3],root_version,catalog_revision,now-2,now+86400,1,2,0)+entry,rolekeys[2])
    trust=header('FORT-PKG-TR-v1');struct.pack_into('<QQQ',trust,24,root_version,catalog_revision,now-1)
    trust[48:64]=ids[3];trust[64:96]=H(catalog);struct.pack_into('<I',trust,96,len(catalog));trust[256:256+len(catalog)]=catalog;trust=sealed(trust)
    state=header('FORT-PKG-ST-v1');struct.pack_into('<Q',state,24,1);state[32:48]=ids[3];state[48:80]=H(trust);state=sealed(state)
    marker=header('FORT-PKG-ENR-v1',512);struct.pack_into('<Q',marker,24,epoch);marker[32:112]=b''.join(ids);marker[112:144]=profile
    for i in range(3):marker[144+8*i:152+8*i]=rows[i+1][4:12]
    struct.pack_into('<II',marker,168,2,1);marker=sealed(marker)
    owners=header('FORT-PKG-OWN-v1',8192);struct.pack_into('<Q',owners,24,epoch);struct.pack_into('<Q',owners,48,1)
    owners[56:104]=ids[4]+ids[0]+ids[3];owners[104:136]=profile;owners=sealed(owners)
    grant=header('FORT-PKG-GRT-v1');struct.pack_into('<QQ',grant,24,1,epoch)
    grant[56:104]=ids[4]+ids[0]+ids[3];grant[104:136]=profile;grant[168:232]=H(state)*2
    grant[232:264]=H(trust);grant[264:296]=H(owners);struct.pack_into('<I',grant,344,1)
    grant[352:360]=rows[3][4:12];struct.pack_into('<4Q',grant,360,1,1,root_version,catalog_revision);grant=sealed(grant)
    origin=header('FORT-ACC-ORG-v1');struct.pack_into('<QQQ',origin,24,1,epoch,1)
    origin[48:80]=ids[4]+ids[0];origin[80:112]=account_profile;origin=sealed(origin)
    account=header('FORT-ACC-GEN-v1',version=3);struct.pack_into('<QQQ',account,24,1,1,epoch)
    account[48:80]=ids[4]+ids[0];account[80:112]=account_profile;account[112:144]=H(origin);struct.pack_into('<Q',account,144,1)
    for i,(path,content) in enumerate(FILES.items()):
        if audit[17+i]['content_sha256']!=H(content).hex():raise ValueError('credential baseline mismatch')
        account[328+32*i:360+32*i]=H(content);struct.pack_into('<I',account,424+4*i,len(content))
    struct.pack_into('<Q',account,440,1);account[448:544]=H(grant)+H(trust)+H(state);account=sealed(account)
    en=bytearray(2464);en[:16]=b'FORT-PKG-ENR-v2\0';struct.pack_into('<IHHQII',en,16,2656,2,2,epoch,2,BS)
    en[40:120]=b''.join(ids);en[120:344]=H(marker)+profile+kernel+bootstrap+H(trust)+H(platform+struct.pack('<Q',base))+account_profile
    struct.pack_into('<QIHHIHHII',en,344,8192,512,16,11,8192,16,32,64,1)
    en[416:2464]=b''.join(rows[:16]);en=signed(en,rolekeys[1])
    parts=[en,marker,grant,owners,account,origin,state,trust]
    for name,data in zip(NAMES,parts):p=out/name;p.write_bytes(data);p.chmod(0o600)
    spec={'profile':'fortress-pkg2-test-only-v1','epoch':epoch,'issued':now-1,'expires':now+86400,
          'uuid':[x.hex() for x in ids],'resource_profile':profile.hex(),'platform':platform.hex(),
          'kernel':kernel.hex(),'bootstrap':bootstrap.hex(),'root_hash':H(root_bytes).hex(),'root_version':root_version,
          'catalog_hash':H(catalog).hex(),'catalog_revision':catalog_revision,'base_version':base,
          'selector_id':H(b'fresh TEST enrollment selector')[:16].hex(),
          'records':{name:str(out/name) for name in NAMES}}
    sp=out/'spec.json';sp.write_text(json.dumps(spec,indent=2)+'\n')
    selector,bundle,producer_audit=prepare(sp,seed_roles['selector'][:2])
    policy=b''.join(b''.join(PUB(k) for k in group).ljust(256,b'\0') for group in rolekeys)
    policy+=struct.pack('<6I',3,2,3,2,3,2)+b''.join(ids)+profile+platform+kernel+bootstrap+H(root_bytes)+H(catalog)+H(selector)+struct.pack('<5Q',now,epoch,root_version,catalog_revision,base)
    assert len(policy)==1136
    return dict(parts=parts,witness=witness,identities=audit,selector=selector,bundle=bundle,
                policy=policy,audit=producer_audit,root_authority=root_bytes)

def write_fresh_records(image,prepared,event=lambda label:None):
    """In-place writes to preallocated files of a caller-owned fresh fixture.

    No allocation, pathname publication or metadata edits. Caller must retain
    the original inventory and verify the whole image/metadata afterwards.
    """
    image=pathlib.Path(image)
    if image.is_symlink() or not image.is_file() or image.stat().st_nlink!=1:raise ValueError('exclusive regular fixture required')
    witness,audit=inventory(image)
    if witness!=prepared['witness']:raise ValueError('fixture identity changed')
    destinations=(16,0,1,2,5,4,6,7)
    for index in destinations:
        if audit[index]['content_sha256']!=H(b'P'*audit[index]['size']).hex():raise ValueError('fresh placeholder records required')
    with image.open('r+b') as f:
        for part,index in zip(prepared['parts'],destinations):
            row=witness[32+128*index:32+128*(index+1)]
            if len(part)!=struct.unpack_from('<Q',row,16)[0]:raise ValueError('preallocation size mismatch')
            at=0
            for j in range(struct.unpack_from('<H',row,32)[0]):
                lo,n,physical=struct.unpack_from('<IIQ',row,40+16*j)
                if lo*BS!=at:raise ValueError('non-dense map')
                take=min(n*BS,len(part)-at);f.seek(physical*BS);f.write(part[at:at+take]);at+=take
            if at!=len(part):raise ValueError('short fixture write')
            f.flush();os.fsync(f.fileno());event('record-'+str(index)+'-fsynced')
        f.flush();os.fsync(f.fileno());event('root-fsynced')
