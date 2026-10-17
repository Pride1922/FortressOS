#include <stdio.h>
#include <assert.h>
#include "crypto.h"
#include "ssh_hostkey.h"
#include "string.h"
#include "syscall_abi.h"
static unsigned calls,wipes;
static unsigned fail_call;
int64_t crypto_test_getrandom(void *p,size_t n,uint64_t flags) {
    assert(!flags && n==32);calls++;
    memset(p,0x42,n); /* Public deterministic fixture, never production entropy. */
    return calls==fail_call ? SYSCALL_EIO : (int64_t)n;
}
void crypto_test_wiped(const void *p,size_t n) {
    const uint8_t *v=p;for(size_t i=0;i<n;i++) assert(!v[i]);wipes++;
}
static void wiped(const ssh_hostkey_workspace *w) {
    const uint8_t *p=(const uint8_t *)w;for(size_t i=0;i<sizeof(*w);i++) assert(!p[i]);
}
int main(void) {
    static ssh_hostkey_workspace w;
    uint8_t seed[32]={1},pk[32],sk[64],result[64];
    char text[1024],copy[1024];size_t n=99;
    assert(!crypto_ed25519_from_seed(seed,pk,sk));
    assert(!ssh_hostkey_encode(sk,0x12345678,text,sizeof(text),&n,&w));wiped(&w);
    assert(n<1024 && text[n]==0);
    assert(!ssh_hostkey_decode(text,n,result,&w));assert(!memcmp(result,sk,64));wiped(&w);
    for(size_t i=0;i<n;i++) {
        memset(result,0x5a,64);memset(&w,0xaa,sizeof(w));
        assert(ssh_hostkey_decode(text,i,result,&w));wiped(&w);
        for(unsigned j=0;j<64;j++) assert(result[j]==0x5a);
    }
    memcpy(copy,text,n+1);size_t old=99;
    assert(ssh_hostkey_encode(sk,0,copy,1,&old,&w));assert(old==99 && !memcmp(text,copy,n+1));wiped(&w);
    sk[32]^=1;assert(ssh_hostkey_encode(sk,0,copy,sizeof(copy),&old,&w));assert(old==99);sk[32]^=1;
    assert(ssh_hostkey_encode(sk,0,(char *)&w,sizeof(w),&old,&w));
    /* Hostile parser mutation, length and arbitrary bytes under ASan/UBSan. */
    uint32_t rng=1;
    for(unsigned i=0;i<10000;i++) {
        memcpy(copy,text,n);rng=rng*1664525u+1013904223u;
        copy[rng%n]^=(char)((rng>>24)|1);
        memset(result,0x5a,64);int r=ssh_hostkey_decode(copy,n,result,&w);wiped(&w);
        if(r) for(unsigned j=0;j<64;j++) assert(result[j]==0x5a);
    }
    for(unsigned failure=1;failure<=2;failure++) {
        calls=0;fail_call=failure;memset(copy,0x5a,sizeof(copy));old=99;
        assert(ssh_hostkey_generate(copy,sizeof(copy),&old,&w)==SYSCALL_EIO);
        assert(old==99 && calls==failure);wiped(&w);
        for(unsigned j=0;j<sizeof(copy);j++) assert(copy[j]==0x5a);
    }
    calls=0;fail_call=0;
    assert(!ssh_hostkey_generate(copy,sizeof(copy),&old,&w));assert(calls==2);wiped(&w);
    assert(!ssh_hostkey_decode(copy,old,result,&w));wiped(&w);
    assert(!ssh_hostkey_public(pk,copy,sizeof(copy),&old,&w));wiped(&w);
    assert(!ssh_hostkey_fingerprint(pk,copy,sizeof(copy),&old,&w));assert(old==50);wiped(&w);
    assert(wipes);puts("PASS hostkey codec sanitizer/truncation/mutation/entropy/failure-output/wipe gates");
}
