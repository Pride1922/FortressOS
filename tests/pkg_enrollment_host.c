#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "tests/pkg_enrollment_candidate.h"
static uint8_t data[PE_MAX+1],good[PE_MAX],slot[PE_SLOT+1],encoded[PE_SLOT];
static pe_policy policy;
static uint64_t le(const uint8_t *p,unsigned n){
    uint64_t v=0;for(unsigned i=0;i<n;i++)v|=(uint64_t)p[i]<<(8*i);return v;
}
static void read_exact(FILE *f,void *p,size_t n){assert(fread(p,1,n,f)==n);}
int main(int argc,char **argv){
    assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);
    unsigned descriptors=0,origins=0,truncations=0,mutations=0;
    pe_enrollment good_en={0};
    for(;;){
        uint8_t h[6],rules[384];size_t got=fread(h,1,6,f);
        if(!got){assert(!ferror(f));break;}assert(got==6);
        size_t n=(size_t)le(h+2,4);assert(n<=sizeof(data));
        if(h[0]==0||h[0]==2){
            read_exact(f,rules,sizeof(rules));read_exact(f,data,n);
            memcpy(policy.keys,rules,256);memcpy(policy.uuid,rules+256,80);
            memcpy(policy.descriptor_sha256,rules+336,32);
            policy.key_count=(unsigned)le(rules+368,4);policy.threshold=(unsigned)le(rules+372,4);
            policy.epoch=le(rules+376,8);
            pe_enrollment out,before;memset(&out,0xa5,sizeof(out));memcpy(&before,&out,sizeof(out));
            int r=pe_verify(data,n,&policy,&out);
            if((r==0)!=h[1]){fprintf(stderr,"descriptor %u result %d expected %u\n",descriptors,r,h[1]);return 1;}
            if(r)assert(!memcmp(&out,&before,sizeof(out)));
            if(!descriptors){
                assert(!r);memcpy(good,data,n);assert(!pe_verify(good,n,&policy,&good_en));
                for(size_t i=0;i<n;i++){
                    assert(pe_verify(data,i,&policy,&out)<0);truncations++;
                }
                /* A pinned object must reject every one-byte modification. */
                for(size_t i=0;i<n;i++){
                    data[i]^=1;assert(pe_verify(data,n,&policy,&out)<0);data[i]^=1;mutations++;
                }
                assert(pe_verify(NULL,n,&policy,&out)<0);
                assert(pe_verify(data,n,NULL,&out)<0);
                assert(pe_verify(data,n,&policy,NULL)<0);
                assert(pe_verify(data,n,&policy,(pe_enrollment *)data)<0);
                assert(pe_verify(data,n,&policy,(pe_enrollment *)&policy)<0);
                assert(pe_verify((void *)(UINTPTR_MAX-8),n,&policy,&out)<0);
            }
            if(h[0]==2){assert(!r);memcpy(good,data,n);assert(!pe_verify(good,n,&policy,&good_en));}
            descriptors++;
        }else{
            assert(h[0]==1&&good_en.wire&&n<=sizeof(slot));read_exact(f,slot,n);
            pe_origin out,before;memset(&out,0xa5,sizeof(out));memcpy(&before,&out,sizeof(out));
            memset(encoded,0xa5,sizeof(encoded));
            int r=pe_origin_decode(slot,n,&good_en,7,3,&out);
            int e=pe_origin_encode(slot,n,&good_en,7,3,encoded,sizeof(encoded));
            if((r==0)!=h[1]||(e==0)!=h[1]){
                fprintf(stderr,"origin %u decode %d encode %d expected %u\n",origins,r,e,h[1]);return 1;
            }
            if(r){
                assert(!memcmp(&out,&before,sizeof(out)));
                for(size_t i=0;i<sizeof(encoded);i++)assert(encoded[i]==0xa5);
            }else{
                assert(!memcmp(slot,encoded,n)&&out.sequence==7&&out.operation_generation==3);
                assert(pe_origin_decode(slot,n,&good_en,8,3,&out)<0);
                assert(pe_origin_decode(slot,n,&good_en,7,4,&out)<0);
                assert(pe_origin_decode(slot,n,&good_en,0,3,&out)<0);
                assert(pe_origin_decode(slot,n,&good_en,7,UINT64_MAX,&out)<0);
                assert(pe_origin_decode(slot,n,NULL,7,3,&out)<0);
                assert(pe_origin_decode(slot,n,&good_en,7,3,(pe_origin *)slot)<0);
                assert(pe_origin_encode(slot,n,&good_en,7,3,slot,n)<0);
                assert(pe_origin_encode(slot,n,&good_en,7,3,encoded,PE_SLOT-1)<0);
                if(!origins){
                    for(size_t i=0;i<n;i++){assert(pe_origin_decode(slot,i,&good_en,7,3,&out)<0);truncations++;}
                    for(size_t i=0;i<n;i++){
                        slot[i]^=1;assert(pe_origin_decode(slot,n,&good_en,7,3,&out)<0);slot[i]^=1;mutations++;
                    }
                }
            }
            origins++;
        }
    }
    assert(!fclose(f));assert(descriptors>30&&origins>30);
    printf("PASS descriptors=%u origins=%u truncations=%u mutations=%u\n",descriptors,origins,truncations,mutations);
}
