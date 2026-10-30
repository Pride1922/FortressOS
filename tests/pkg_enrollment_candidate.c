#include "pkg_enrollment_candidate.h"
#include "crypto.h"
static uint64_t le(const uint8_t *p,unsigned n){
    uint64_t v=0;for(unsigned i=0;i<n;i++)v|=(uint64_t)p[i]<<(8*i);return v;
}
static int zero(const uint8_t *p,size_t n){
    for(size_t i=0;i<n;i++)if(p[i])return 0;
    return 1;
}
static int cmp(const uint8_t *a,const uint8_t *b,size_t n){
    for(size_t i=0;i<n;i++)if(a[i]!=b[i])return a[i]<b[i]?-1:1;
    return 0;
}
static int range(const void *p,size_t n){return p && n<=UINTPTR_MAX-(uintptr_t)p;}
static int apart(const void *a,size_t an,const void *b,size_t bn){
    return range(a,an)&&range(b,bn)&&
        ((uintptr_t)a+an<=(uintptr_t)b||(uintptr_t)b+bn<=(uintptr_t)a);
}
static int extent(const uint8_t *x,uint64_t blocks){
    uint64_t lo=le(x+8,8),len=le(x+4,4);
    return len && lo && lo<blocks && len<=blocks-lo &&
        le(x,4)+len<=UINT32_MAX;
}
static int overlap(uint64_t a,uint64_t an,uint64_t b,uint64_t bn){
    return a<b+bn && b<a+an; /* endpoints checked before use */
}
static int descriptor(const uint8_t *p,size_t n){
    static const uint8_t domain[16]="FORT-PKG-ENR-v2";
    if(!range(p,n)||n<PE_BODY||n>PE_MAX||cmp(p,domain,16)||le(p+16,4)!=n||
       le(p+20,2)!=2||!le(p+22,2)||le(p+22,2)>8||
       n!=PE_BODY+96*le(p+22,2)||!le(p+24,8)||le(p+24,8)==UINT64_MAX||
       le(p+32,4)!=2)return -1;
    uint64_t bs=le(p+36,4),blocks=le(p+344,8),inodes=le(p+352,4);
    if((bs!=1024&&bs!=2048&&bs!=4096)||!blocks||blocks>(UINT64_C(1)<<40)/bs||
       inodes<32||inodes>1048576||le(p+356,2)!=16||le(p+358,2)!=11||
       le(p+360,4)!=8192||le(p+364,2)!=16||le(p+366,2)!=32||
       le(p+368,4)!=64||le(p+372,4)!=1||!zero(p+376,40))return -1;
    for(unsigned i=0;i<5;i++)if(zero(p+40+16*i,16))return -1;
    for(unsigned i=0;i<7;i++)if(zero(p+120+32*i,32))return -1;
    for(unsigned i=0;i<16;i++){
        const uint8_t *r=p+416+128*i;
        unsigned dir=i==3||i>=8,mode=(unsigned)le(r+12,4),count=(unsigned)le(r+32,2);
        uint64_t size=le(r+16,8),logical=0;
        if(le(r,2)!=i+1||le(r+2,2)!=(dir?2:1)||le(r+4,4)<11||le(r+4,4)>inodes||
           !le(r+8,4)||(dir?(mode!=0700&&mode!=0755):mode!=0600)||
           !size||size>1048576||!count||count>4||!zero(r+34,6)||
           !zero(r+40+16*count,88-16*count))return -1;
        for(unsigned j=0;j<count;j++){
            const uint8_t *x=r+40+16*j;
            if(!extent(x,blocks)||le(x,4)!=logical)return -1;
            logical+=le(x+4,4);
            for(unsigned a=0;a<=i;a++){
                const uint8_t *q=p+416+128*a;
                if(a<i&&le(q+4,4)==le(r+4,4))return -1;
                unsigned limit=a==i?j:(unsigned)le(q+32,2);
                for(unsigned b=0;b<limit;b++){
                    const uint8_t *y=q+40+16*b;
                    if(overlap(le(x+8,8),le(x+4,4),le(y+8,8),le(y+4,4)))return -1;
                }
            }
        }
        if(logical!=(size+bs-1)/bs||logical!=le(r+24,8))return -1;
    }
    return 0;
}
int pe_verify(const uint8_t *p,size_t n,const pe_policy *policy,pe_enrollment *out){
    if(!range(policy,sizeof(*policy))||!range(out,sizeof(*out))||
       !apart(out,sizeof(*out),p,n)||!apart(out,sizeof(*out),policy,sizeof(*policy))||
       !policy->key_count||policy->key_count>8||!policy->threshold||
       policy->threshold>policy->key_count||descriptor(p,n)||le(p+24,8)!=policy->epoch||
       cmp(p+40,policy->uuid[0],80))return -1;
    for(unsigned i=0;i<policy->key_count;i++){
        if(zero(policy->keys[i],32))return -1;
        for(unsigned j=0;j<i;j++)if(!cmp(policy->keys[i],policy->keys[j],32))return -1;
    }
    uint8_t hash[32],id[32];
    if(crypto_sha256(p,n,hash)||crypto_verify32(hash,policy->descriptor_sha256))return -1;
    unsigned count=(unsigned)le(p+22,2);
    for(unsigned i=0;i<count;i++){
        const uint8_t *s=p+PE_BODY+96*i;
        if(i&&cmp(s-96,s,32)>=0)return -1;
        unsigned k=0;
        for(;k<policy->key_count;k++){
            if(crypto_sha256(policy->keys[k],32,id))return -1;
            if(!crypto_verify32(id,s))break;
        }
        if(k==policy->key_count||crypto_ed25519_verify(s+32,policy->keys[k],p,PE_BODY))return -1;
    }
    if(count<policy->threshold)return -1;
    *out=(pe_enrollment){p,n};return 0;
}
int pe_origin_decode(const uint8_t *p,size_t n,const pe_enrollment *en,
                     uint64_t sequence,uint64_t account_generation,pe_origin *out){
    static const uint8_t domain[16]="FORT-ACC-ORG-v1";
    if(!range(en,sizeof(*en))||!range(out,sizeof(*out))||
       !apart(out,sizeof(*out),p,n)||!apart(out,sizeof(*out),en,sizeof(*en))||
       !apart(out,sizeof(*out),en->wire,en->length)||descriptor(en->wire,en->length)||
       !range(p,n)||n!=PE_SLOT||cmp(p,domain,16)||le(p+16,4)!=PE_SLOT||
       le(p+20,2)!=1||le(p+22,2)>1||!sequence||sequence==UINT64_MAX||
       !account_generation||account_generation==UINT64_MAX||
       le(p+24,8)!=sequence||le(p+32,8)!=le(en->wire+24,8)||
       le(p+40,8)!=account_generation||cmp(p+48,en->wire+104,16)||
       cmp(p+64,en->wire+40,16)||cmp(p+80,en->wire+312,32))return -1;
    uint8_t hash[32];
    if(crypto_sha256(p,992,hash)||crypto_verify32(hash,p+992))return -1;
    unsigned active=(unsigned)le(p+22,2),role=(unsigned)le(p+112,2),phase=(unsigned)le(p+114,2);
    unsigned ec=(unsigned)le(p+152,2),rc=(unsigned)le(p+154,2);
    uint64_t size=le(p+144,8),blocks=le(en->wire+344,8),bs=le(en->wire+36,4);
    if(!active){if(!zero(p+112,880))return -1;}
    else{
        unsigned private_role=(role>=3&&role<=5)||role>=9;
        const uint8_t *parent=en->wire+416+128*(private_role?13:9);
        unsigned mode=(role==0||role==1||role==6||role==7)?0644:0600;
        if(role>=11||phase>2||le(p+116,4)<11||le(p+116,4)>le(en->wire+352,4)||
           !le(p+120,4)||le(p+124,4)!=le(parent+4,4)||le(p+128,4)!=le(parent+8,4)||
           le(p+132,4)!=mode||le(p+136,4)||le(p+140,4)||size>8192||
           (role>=9&&size!=0&&size!=160)||(size&&(!ec||!rc))||
           (!size&&(ec||rc))||ec>16||rc>32||!zero(p+156,4)||
           !zero(p+160+16*ec,256-16*ec)||!zero(p+416+16*rc,576-16*rc))return -1;
        for(unsigned i=0;i<16;i++)if(le(p+116,4)==le(en->wire+416+128*i+4,4))return -1;
        uint64_t logical=0,total=0;
        for(unsigned i=0;i<rc;i++){
            const uint8_t *x=p+416+16*i;
            uint64_t lo=le(x,8),len=le(x+8,4);
            if(!lo||lo>=blocks||!len||len>blocks-lo||!zero(x+12,4)||
               (i&&le(x-16,8)+le(x-8,4)>=lo))return -1;
            total+=len;
            for(unsigned a=0;a<16;a++){
                const uint8_t *r=en->wire+416+128*a;
                for(unsigned b=0;b<le(r+32,2);b++){
                    const uint8_t *y=r+40+16*b;
                    if(overlap(lo,len,le(y+8,8),le(y+4,4)))return -1;
                }
            }
        }
        for(unsigned i=0;i<ec;i++){
            const uint8_t *x=p+160+16*i;unsigned found=0;
            if(!extent(x,blocks)||le(x,4)!=logical)return -1;
            logical+=le(x+4,4);
            for(unsigned j=0;j<i;j++){
                const uint8_t *y=p+160+16*j;
                if(overlap(le(x+8,8),le(x+4,4),le(y+8,8),le(y+4,4)))return -1;
            }
            for(unsigned j=0;j<rc;j++){
                const uint8_t *r=p+416+16*j;
                if(le(x+8,8)>=le(r,8)&&le(x+8,8)+le(x+4,4)<=le(r,8)+le(r+8,4))found++;
            }
            if(found!=1)return -1;
        }
        uint64_t d=(size+bs-1)/bs;
        if(logical!=d||total<d||total>d+(d>4))return -1;
    }
    *out=(pe_origin){.sequence=sequence,.operation_generation=account_generation,.size=size,
        .inode=(uint32_t)le(p+116,4),.generation=(uint32_t)le(p+120,4),
        .parent=(uint32_t)le(p+124,4),.parent_generation=(uint32_t)le(p+128,4),
        .active=active,.role=role,.phase=phase,.extents=ec,.ranges=rc,.wire=p};return 0;
}
int pe_origin_encode(const uint8_t *p,size_t n,const pe_enrollment *en,uint64_t seq,uint64_t gen,
                     uint8_t *out,size_t capacity){
    pe_origin view;
    if(!range(en,sizeof(*en))||capacity!=PE_SLOT||!apart(out,capacity,p,n)||
       !apart(out,capacity,en,sizeof(*en))||!apart(out,capacity,en->wire,en->length)||
       pe_origin_decode(p,n,en,seq,gen,&view))return -1;
    for(size_t i=0;i<n;i++)out[i]=p[i];
    return 0;
}
