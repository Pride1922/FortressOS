#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "pkg_trust_floor.h"
#include "string.h"
static void read_exact(const char *path,void *p,size_t n){FILE *f=fopen(path,"rb");assert(f&&fread(p,1,n,f)==n&&fgetc(f)==EOF&&!fclose(f));}
int main(int argc,char **argv){
    assert(argc==5);ps_policy p;uint8_t wire[3072];tf_floor f[3],bad,unchanged;read_exact(argv[1],&p,sizeof p);read_exact(argv[2],wire,sizeof wire);
    int expected=atoi(argv[3]);int r=0;for(unsigned i=0;i<3;i++){memset(&f[i],0xa5,sizeof f[i]);unchanged=f[i];int x=tf_decode(wire+1024*i,&p,&f[i]);if(x){assert(!memcmp(&unchanged,&f[i],sizeof unchanged));r=x;}}
    if(!expected){assert(r);puts("PASS malformed/signed refusal with unchanged output");return 0;}
    assert(!r);assert(!tf_boot(f,f,f+1,f+2,&p));assert(!tf_boot(f+1,f,f+1,f+2,&p));assert(!tf_boot(f+2,f,f+1,f+2,&p));assert(!tf_boot(f+2,f+2,f+1,f+2,&p));assert(tf_boot(f,f+2,f+1,f+2,&p));
    tf_observed o;memset(&o,0,sizeof o);memcpy(o.counter,f[2].counter,sizeof o.counter);memcpy(o.root,f[2].root,32);memcpy(o.catalog,f[2].catalog,32);memcpy(o.artifact,f[2].artifact,32);assert(!tf_admit(f+2,&o));
    for(unsigned i=0;i<5;i++){o.counter[i]--;assert(tf_admit(f+2,&o));o.counter[i]++;bad=f[2];bad.epoch++;bad.counter[i]--;assert(tf_transition(f+2,&bad,p.now));}
    for(unsigned i=0;i<3;i++){uint8_t *h=i==0?o.root:i==1?o.catalog:o.artifact;h[0]^=1;assert(tf_admit(f+2,&o));h[0]^=1;}
    bad=f[2];bad.root[0]^=1;assert(tf_transition(f+1,&bad,p.now));bad=f[2];bad.catalog[0]^=1;assert(tf_transition(f+1,&bad,p.now));
    bad=f[2];bad.kernel[0]^=1;assert(tf_boot(f,f,f+1,&bad,&p));bad=f[2];bad.bootstrap[0]^=1;assert(tf_boot(f,f,f+1,&bad,&p));bad=f[2];bad.contract=TF_CONTRACT+1;assert(tf_boot(f,f,f+1,&bad,&p));
    bad=f[2];bad.selector[0]^=1;assert(tf_boot(f,f,f+1,&bad,&p));bad=f[2];bad.epoch++;assert(tf_transition(f+1,&bad,p.now));assert(tf_transition(f,f+1,f[1].expires));assert(tf_transition(f,f+1,f[1].issued-1));
    bad=f[1];bad.wire_hash[0]^=1;assert(tf_boot(&bad,f,f+1,f+2,&p));bad=f[2];bad.identity[0]^=1;assert(tf_boot(&bad,f,f+1,f+2,&p));
    memset(&bad,0xa5,sizeof bad);unchanged=bad;assert(tf_decode(wire,&p,NULL));assert(tf_decode(wire,&p,(tf_floor *)wire));
    ps_policy invalid=p;memcpy(invalid.selector_keys[1],invalid.selector_keys[0],32);assert(tf_decode(wire,&invalid,&bad)&&!memcmp(&bad,&unchanged,sizeof bad));
    for(unsigned i=0;i<1024;i++){uint8_t b[1024];memcpy(b,wire,1024);b[i]^=1;memset(&bad,0xa5,sizeof bad);unchanged=bad;assert(tf_decode(b,&p,&bad)&&!memcmp(&bad,&unchanged,sizeof bad));}
    (void)argv[4];puts("PASS signed checkpoints, monotonic counters/hash binding, reboot minimum, measurements, contract, expiry and 1024 byte corruptions");return 0;
}
