#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crypto.h"
#include "syscall_abi.h"
#include "crypto_vectors.h"

static size_t wipes,seed_wipes,entropy_calls,hash_wipes;
static int64_t entropy_result=32;
void crypto_test_wiped(const void *p,size_t n) {
    for(size_t i=0;i<n;i++) assert(!((const uint8_t *)p)[i]);
    wipes++; if(n==32) seed_wipes++; if(n==64) hash_wipes++;
}
int64_t crypto_test_getrandom(void *out,size_t n,uint64_t flags) {
    assert(n==32 && !flags); entropy_calls++;
    memset(out,0x73,32); /* even a partial/error result must get wiped */
    return entropy_result;
}
static void unhex(const char *hex,uint8_t *out,size_t n) {
    for(size_t i=0;i<n;i++) {unsigned x; assert(sscanf(hex+2*i,"%2x",&x)==1);out[i]=(uint8_t)x;}
}
static void eqhex(const uint8_t *got,const char *hex,size_t n) {
    uint8_t expected[128]; assert(n<=sizeof(expected));unhex(hex,expected,n);assert(!memcmp(got,expected,n));
}
static void hashes(void) {
    uint8_t out[64];
    assert(!crypto_sha256(NULL,0,out));
    eqhex(out,"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",32);
    assert(!crypto_sha256((const uint8_t *)"abc",3,out));
    eqhex(out,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",32);
    assert(!crypto_sha256((const uint8_t *)"a",1,out));
    eqhex(out,"ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb",32);
    assert(!crypto_sha512(NULL,0,out));
    eqhex(out,"cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
              "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e",64);
    assert(!crypto_sha512((const uint8_t *)"abc",3,out));
    eqhex(out,"ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
              "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",64);
    crypto_sha256_ctx a; crypto_sha512_ctx b;
    assert(!crypto_sha256_init(&a));assert(!crypto_sha512_init(&b));
    uint8_t chunk[1000];memset(chunk,'a',sizeof(chunk));
    for(unsigned i=0;i<1000;i++) {
        assert(!crypto_sha256_update(&a,chunk,sizeof(chunk)));
        assert(!crypto_sha512_update(&b,chunk,sizeof(chunk)));
    }
    assert(!crypto_sha256_final(&a,out));
    eqhex(out,"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",32);
    assert(!crypto_sha512_final(&b,out));
    eqhex(out,"e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
              "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b",64);
    assert(crypto_sha256_final(&a,out)<0);assert(crypto_sha512_update(&b,NULL,0)<0);
    puts("PASS SHA-256/SHA-512 RFC6234 empty/abc/a/million-a and streaming lifecycle");
}
static void hmac_vectors(void) {
    uint8_t out[32];
    for(size_t i=0;i<sizeof(hm_vectors)/sizeof(*hm_vectors);i++) {
        const hm_vector *v=&hm_vectors[i];
        assert(!crypto_hmac_sha256(v->key,v->kn,v->msg,v->n,out));
        assert(!memcmp(out,v->out,v->on));
    }
    puts("PASS HMAC-SHA256 all seven RFC4231 cases (case5 verifies 128-bit prefix)");
}
static void ed_vectors_test(void) {
    for(size_t i=0;i<sizeof(ed_vectors)/sizeof(*ed_vectors);i++) {
        const ed_vector *v=&ed_vectors[i];
        uint8_t pk[32],sk[64],sig[64],bad[64],msg[1024];
        assert(!crypto_ed25519_from_seed(v->seed,pk,sk));assert(!memcmp(pk,v->pk,32));
        assert(!memcmp(sk,v->seed,32));assert(!memcmp(sk+32,pk,32));
        size_t before_hash=hash_wipes;
        assert(!crypto_ed25519_sign(sig,sk,v->msg,v->n));assert(!memcmp(sig,v->sig,64));
        /* Two hash/reduction buffers, expanded scalar/prefix and staged result. */
        assert(hash_wipes>=before_hash+4);
        assert(!crypto_ed25519_verify(sig,pk,v->msg,v->n));
        for(unsigned j=0;j<64;j++) {
            memcpy(bad,sig,64); bad[j]^=1; assert(crypto_ed25519_verify(bad,pk,v->msg,v->n)<0);
        }
        pk[0]^=1; assert(crypto_ed25519_verify(sig,pk,v->msg,v->n)<0); pk[0]^=1;
        memcpy(msg,v->msg,v->n); msg[v->n]=1;
        assert(crypto_ed25519_verify(sig,pk,msg,v->n+1)<0);
        /* S=L is noncanonical, even with otherwise valid R and public key. */
        memcpy(bad,sig,64);
        unhex("edd3f55c1a631258d69cf7a2def9de1400000000000000000000000000000010",bad+32,32);
        assert(crypto_ed25519_verify(bad,pk,v->msg,v->n)<0);
        memset(bad,0,64);memset(pk,0,32);pk[0]=1;bad[0]=1;
        assert(crypto_ed25519_verify(bad,pk,NULL,0)<0);
        memset(pk,255,32);pk[0]=0xed;pk[31]=0x7f;
        assert(crypto_ed25519_verify(v->sig,pk,v->msg,v->n)<0);
    }
    puts("PASS all five pure-Ed25519 RFC8032 vectors and 320 signature mutations/noncanonical/low-order cases");
}
static void x_vectors(void) {
    uint8_t alice[32],bob[32],ap[32],bp[32],shared[32],ask[32],bsk[32];
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a",alice,32);
    unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb",bob,32);
    assert(!crypto_x25519_from_seed(alice,ask,ap));assert(!crypto_x25519_from_seed(bob,bsk,bp));
    eqhex(ap,"8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a",32);
    eqhex(bp,"de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f",32);
    assert(!crypto_x25519(shared,ask,bp));
    eqhex(shared,"4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742",32);
    uint8_t other[32]; assert(!crypto_x25519(other,bsk,ap));assert(!memcmp(other,shared,32));
    const char *raw[][3]={
        {"a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
         "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
         "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"},
        {"4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d",
         "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493",
         "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957"}};
    for(unsigned i=0;i<2;i++) {
        unhex(raw[i][0],alice,32);unhex(raw[i][1],bp,32);
        assert(!crypto_x25519(shared,alice,bp));eqhex(shared,raw[i][2],32);
    }
    const char *low[]={
        "0000000000000000000000000000000000000000000000000000000000000000",
        "0100000000000000000000000000000000000000000000000000000000000000",
        "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
        "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f"};
    for(unsigned i=0;i<7;i++) for(unsigned high=0;high<2;high++) {
        unhex(low[i],bp,32); bp[31]|=high?128:0;memset(shared,0xa5,32);
        assert(crypto_x25519(shared,ask,bp)<0);
        for(unsigned j=0;j<32;j++) assert(shared[j]==0xa5);
    }
    /* RFC7748 iterated scalar multiplication, counts 1 and 1000. */
    memset(alice,0,32);alice[0]=9;memcpy(bp,alice,32);
    for(unsigned i=0;i<1000;i++) {
        assert(!crypto_x25519(shared,alice,bp));memcpy(bp,alice,32);memcpy(alice,shared,32);
        if(!i) eqhex(shared,"422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079",32);
    }
    eqhex(shared,"684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51",32);
    puts("PASS X25519 RFC7748 DH/two raw vectors/1/1000 iterations; 14 low-order encodings preserve output");
}
static void aead_test(void) {
    uint8_t ct[114],plain[114],tag[16],bad[114],key[32],nonce[12],ad[12];
    assert(sizeof(aead_msg)==114 && sizeof(aead_ct)==114);
    assert(!crypto_aead_lock(ct,tag,aead_key,aead_nonce,aead_ad,12,aead_msg,114));
    assert(!memcmp(ct,aead_ct,114));assert(!memcmp(tag,aead_tag,16));
    assert(!crypto_aead_unlock(plain,tag,aead_key,aead_nonce,aead_ad,12,ct,114));
    assert(!memcmp(plain,aead_msg,114));
    for(unsigned i=0;i<16;i++) {
        tag[i]^=1;memset(plain,0xa5,114);
        assert(crypto_aead_unlock(plain,tag,aead_key,aead_nonce,aead_ad,12,ct,114)<0);
        for(unsigned j=0;j<114;j++) assert(plain[j]==0xa5);
        tag[i]^=1;
    }
    for(unsigned i=0;i<114;i++) {
        memcpy(bad,ct,114); bad[i]^=1;memset(plain,0xa5,114);
        assert(crypto_aead_unlock(plain,tag,aead_key,aead_nonce,aead_ad,12,bad,114)<0);
        for(unsigned j=0;j<114;j++) assert(plain[j]==0xa5);
    }
    memcpy(key,aead_key,32);key[0]^=1;memset(plain,0xa5,114);
    assert(crypto_aead_unlock(plain,tag,key,aead_nonce,aead_ad,12,ct,114)<0);
    memcpy(nonce,aead_nonce,12);nonce[0]^=1;
    assert(crypto_aead_unlock(plain,tag,aead_key,nonce,aead_ad,12,ct,114)<0);
    memcpy(ad,aead_ad,12);ad[0]^=1;
    assert(crypto_aead_unlock(plain,tag,aead_key,aead_nonce,ad,12,ct,114)<0);
    for(unsigned j=0;j<114;j++) assert(plain[j]==0xa5);
    memcpy(plain,aead_msg,114);
    assert(!crypto_aead_lock(plain,tag,aead_key,aead_nonce,aead_ad,12,plain,114));
    assert(!memcmp(plain,ct,114));
    tag[0]^=1; assert(crypto_aead_unlock(plain,tag,aead_key,aead_nonce,aead_ad,12,plain,114)<0);
    assert(!memcmp(plain,ct,114));tag[0]^=1;
    assert(!crypto_aead_unlock(plain,tag,aead_key,aead_nonce,aead_ad,12,plain,114));
    assert(!memcmp(plain,aead_msg,114));
    assert(!crypto_aead_lock(NULL,tag,aead_key,aead_nonce,NULL,0,NULL,0));
    assert(!crypto_aead_unlock(NULL,tag,aead_key,aead_nonce,NULL,0,NULL,0));
    puts("PASS RFC8439 2.8.2 ciphertext/tag; all-byte tampering, key/nonce/AD, zero/in-place and no plaintext on failure");
}
static void errors(void) {
    uint8_t b[256],copy[256],pk[32],sk[64];memset(b,0xa5,sizeof(b));memcpy(copy,b,sizeof(b));
    assert(crypto_sha256(NULL,1,b)<0);assert(crypto_sha512(NULL,1,b)<0);
    assert(crypto_sha256((void *)(UINTPTR_MAX-8),32,b)<0);
    assert(crypto_hmac_sha256(NULL,1,b,1,b+128)<0);
    assert(crypto_hmac_sha256(b,1,NULL,1,b+128)<0);
    assert(crypto_hmac_sha256(b,32,b+64,32,b)<0);
    assert(crypto_sha256(b,10,b)<0);assert(crypto_sha512(b,10,b)<0);
    assert(crypto_ed25519_from_seed(b,b,b+64)<0);
    assert(crypto_ed25519_sign(b,b+128,b,10)<0);
    assert(crypto_x25519_from_seed(b,b,b+64)<0);
    assert(crypto_x25519(b,b,b+64)<0);
    assert(crypto_aead_lock(b+1,b+128,b+160,b+200,NULL,0,b,32)<0);
    assert(crypto_aead_lock(b,b+128,b+160,b+200,NULL,0,b,CRYPTO_MAX_AEAD_BYTES+1)<0);
    assert(crypto_aead_lock(b,b+128,b+160,b+200,NULL,1,b,32)<0);
    assert(!memcmp(b,copy,sizeof(b)));
    crypto_sha256_ctx a; crypto_sha512_ctx c; crypto_sha256_init(&a);crypto_sha512_init(&c);
    a.digest.bytes=CRYPTO_MAX_INPUT;c.bytes=CRYPTO_MAX_INPUT;
    assert(crypto_sha256_update(&a,b,1)<0);assert(crypto_sha512_update(&c,b,1)<0);
    assert(a.digest.bytes==CRYPTO_MAX_INPUT && c.bytes==CRYPTO_MAX_INPUT);
    for(unsigned i=0;i<32;i++) { memset(pk,0,32);memset(sk,0,64);sk[i]=1;assert(crypto_verify32(pk,sk)<0); }
    memset(sk,0,32);assert(!crypto_verify32(pk,sk));
    for(unsigned i=0;i<3;i++) {
        entropy_result=i==0 ? SYSCALL_EIO : i==1 ? SYSCALL_EAGAIN : 16;
        memset(pk,0xa5,32);memset(sk,0xa5,64);size_t w=seed_wipes;
        assert(crypto_ed25519_keygen(pk,sk)==(i==1 ? SYSCALL_EAGAIN : SYSCALL_EIO));
        assert(crypto_x25519_keygen(sk,pk)==(i==1 ? SYSCALL_EAGAIN : SYSCALL_EIO));
        for(unsigned j=0;j<32;j++) assert(pk[j]==0xa5);
        for(unsigned j=0;j<64;j++) assert(sk[j]==0xa5);
        assert(seed_wipes>=w+2);
    }
    entropy_result=32;size_t before=entropy_calls;
    assert(!crypto_ed25519_keygen(pk,sk));assert(!crypto_x25519_keygen(sk,pk));
    assert(entropy_calls==before+2 && wipes && seed_wipes);
    puts("PASS argument/alias/overflow/canary/streaming/compare and fail-closed random keygen/wipe hooks");
}
int main(void) {
    hashes();hmac_vectors();ed_vectors_test();x_vectors();aead_test();errors();
    puts("PASS crypto host suite");return 0;
}
