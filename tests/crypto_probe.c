#include "common.h"
#include "crypto.h"
#include "crypto_vectors.h"
#define CHECK(x) do {if(!(x)) {say("CRYPTO FAIL: " #x "\n");return 1;}}while(0)
static void say(const char *s) {tool_write("crypto-probe",s,tool_length(s));}
static bool equal(const uint8_t *a,const uint8_t *b,size_t n) {
    for(size_t i=0;i<n;i++) if(a[i]!=b[i]) return false;
    return true;
}
static unsigned digit(char c) {return c<='9' ? (unsigned)(c-'0') : (unsigned)(c-'a'+10);}
static void unhex(const char *s,uint8_t *b,size_t n) {
    for(size_t i=0;i<n;i++) b[i]=(uint8_t)(digit(s[2*i])*16+digit(s[2*i+1]));
}
static int hash_probe(void) {
    uint8_t hash[64],expected[64];
    CHECK(!crypto_sha256((const uint8_t *)"hello world",11,hash));
    unhex("b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9",expected,32);
    CHECK(equal(hash,expected,32));
    CHECK(!crypto_sha512((const uint8_t *)"abc",3,hash));
    unhex("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
          "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",expected,64);
    CHECK(equal(hash,expected,64));
    for(unsigned i=0;i<7;i++) {
        const hm_vector *v=&hm_vectors[i];
        CHECK(!crypto_hmac_sha256(v->key,v->kn,v->msg,v->n,hash));
        CHECK(equal(hash,v->out,v->on));
    }
    say("CRYPTO hash/HMAC PASS\n");return 0;
}
static int ed_probe(void) {
    uint8_t pk[32],sk[64],sig[64];
    for(unsigned i=0;i<5;i++) {
        const ed_vector *v=&ed_vectors[i];
        CHECK(!crypto_ed25519_from_seed(v->seed,pk,sk));CHECK(equal(pk,v->pk,32));
        CHECK(!crypto_ed25519_sign(sig,sk,v->msg,v->n));CHECK(equal(sig,v->sig,64));
        CHECK(!crypto_ed25519_verify(sig,pk,v->msg,v->n));
        sig[0]^=1;CHECK(crypto_ed25519_verify(sig,pk,v->msg,v->n)<0);
    }
    CHECK(!crypto_ed25519_keygen(pk,sk));
    CHECK(!crypto_ed25519_sign(sig,sk,(const uint8_t *)"hello world",11));
    CHECK(!crypto_ed25519_verify(sig,pk,(const uint8_t *)"hello world",11));
    crypto_wipe(sk,sizeof(sk));say("CRYPTO Ed25519 PASS\n");return 0;
}
static int x_probe(void) {
    uint8_t seed[32],sk[32],pk[32],peer[32],shared[32],expected[32];
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a",seed,32);
    CHECK(!crypto_x25519_from_seed(seed,sk,pk));
    unhex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a",expected,32);
    CHECK(equal(pk,expected,32));
    unhex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f",peer,32);
    CHECK(!crypto_x25519(shared,sk,peer));
    unhex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742",expected,32);
    CHECK(equal(shared,expected,32));
    for(unsigned i=0;i<32;i++) peer[i]=0;
    CHECK(crypto_x25519(shared,sk,peer)<0);CHECK(equal(shared,expected,32));
    CHECK(!crypto_x25519_keygen(sk,pk));
    crypto_wipe(seed,32);crypto_wipe(sk,32);crypto_wipe(shared,32);
    say("CRYPTO X25519 PASS\n");return 0;
}
static int aead_probe(void) {
    uint8_t ct[114],pt[114],tag[16];
    CHECK(!crypto_aead_lock(ct,tag,aead_key,aead_nonce,aead_ad,12,aead_msg,114));
    CHECK(equal(ct,aead_ct,114) && equal(tag,aead_tag,16));
    CHECK(!crypto_aead_unlock(pt,tag,aead_key,aead_nonce,aead_ad,12,ct,114));
    CHECK(equal(pt,aead_msg,114));
    for(unsigned i=0;i<114;i++) pt[i]=0xa5;
    tag[0]^=1;CHECK(crypto_aead_unlock(pt,tag,aead_key,aead_nonce,aead_ad,12,ct,114)<0);
    for(unsigned i=0;i<114;i++) CHECK(pt[i]==0xa5);
    say("CRYPTO AEAD PASS\n");return 0;
}
int crypto_probe_main(int argc,char **argv) {
    if(argc==2 && tool_equal(argv[1],"--unavailable")) {
        uint8_t pk[32],sk[64];
        for(unsigned i=0;i<32;i++) pk[i]=0xa5;
        for(unsigned i=0;i<64;i++) sk[i]=0xa5;
        CHECK(crypto_ed25519_keygen(pk,sk)==SYSCALL_EIO);
        CHECK(crypto_x25519_keygen(sk,pk)==SYSCALL_EIO);
        for(unsigned i=0;i<32;i++) CHECK(pk[i]==0xa5);
        for(unsigned i=0;i<64;i++) CHECK(sk[i]==0xa5);
        say("CRYPTO unavailable PASS\n");return 0;
    }
    if(hash_probe() || ed_probe() || x_probe() || aead_probe()) return 1;
    say("CRYPTO probe PASS\n");return 0;
}
