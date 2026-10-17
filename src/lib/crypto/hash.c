#include "internal.h"
void crypto_wipe(void *p,size_t n) {
    mono_crypto_wipe(p,n);
#ifdef CRYPTO_HOST_TEST
    extern void crypto_test_wiped(const void *,size_t);
    crypto_test_wiped(p,n);
#endif
}
/* Fixed-size upstream comparison; disassembly reviewed for supported compiler.
 * Success/failure itself is public. Do not compose this into a secret predicate. */
int crypto_verify32(const uint8_t a[32],const uint8_t b[32]) {
    if(!cr_range(a,32) || !cr_range(b,32)) return SYSCALL_EINVAL;
    return mono_crypto_verify32(a,b);
}
int crypto_sha256_init(crypto_sha256_ctx *c) {
    if(!cr_range(c,sizeof(*c))) return SYSCALL_EINVAL;
    *c=(crypto_sha256_ctx){0}; digest_init(&c->digest,DIGEST_SHA256); c->active=true; return 0;
}
int crypto_sha256_update(crypto_sha256_ctx *c,const void *p,size_t n) {
    if(!cr_range(c,sizeof(*c)) || !c->active || !cr_range(p,n) ||
       cr_overlap(c,sizeof(*c),p,n) || c->digest.bytes>CRYPTO_MAX_INPUT || n>CRYPTO_MAX_INPUT-c->digest.bytes)
        return SYSCALL_EINVAL;
    return digest_update(&c->digest,p,n) ? 0 : SYSCALL_EINVAL;
}
int crypto_sha256_final(crypto_sha256_ctx *c,uint8_t out[32]) {
    if(!cr_range(c,sizeof(*c)) || !c->active || !cr_output(out,32,c,sizeof(*c))) return SYSCALL_EINVAL;
    digest_final(&c->digest,out); crypto_wipe(c,sizeof(*c)); return 0;
}
int crypto_sha256(const uint8_t *p,size_t n,uint8_t out[32]) {
    if(!cr_output(out,32,p,n)) return SYSCALL_EINVAL;
    crypto_sha256_ctx c; crypto_sha256_init(&c);
    int r=crypto_sha256_update(&c,p,n);
    if(!r) r=crypto_sha256_final(&c,out);
    crypto_wipe(&c,sizeof(c)); return r;
}
_Static_assert(sizeof(mono_crypto_sha512_ctx)<=256,"opaque SHA512 storage");
int crypto_sha512_init(crypto_sha512_ctx *c) {
    if(!cr_range(c,sizeof(*c))) return SYSCALL_EINVAL;
    mono_crypto_sha512_ctx m; mono_crypto_sha512_init(&m);
    *c=(crypto_sha512_ctx){0}; memcpy(c->state,&m,sizeof(m)); c->active=true;
    crypto_wipe(&m,sizeof(m)); return 0;
}
int crypto_sha512_update(crypto_sha512_ctx *c,const void *p,size_t n) {
    if(!cr_range(c,sizeof(*c)) || !c->active || !cr_range(p,n) ||
       cr_overlap(c,sizeof(*c),p,n) || c->bytes>CRYPTO_MAX_INPUT || n>CRYPTO_MAX_INPUT-c->bytes)
        return SYSCALL_EINVAL;
    mono_crypto_sha512_ctx m; memcpy(&m,c->state,sizeof(m));
    mono_crypto_sha512_update(&m,p,n); memcpy(c->state,&m,sizeof(m)); c->bytes+=n;
    crypto_wipe(&m,sizeof(m)); return 0;
}
int crypto_sha512_final(crypto_sha512_ctx *c,uint8_t out[64]) {
    if(!cr_range(c,sizeof(*c)) || !c->active || !cr_output(out,64,c,sizeof(*c))) return SYSCALL_EINVAL;
    mono_crypto_sha512_ctx m; memcpy(&m,c->state,sizeof(m));
    mono_crypto_sha512_final(&m,out); crypto_wipe(&m,sizeof(m)); crypto_wipe(c,sizeof(*c)); return 0;
}
int crypto_sha512(const uint8_t *p,size_t n,uint8_t out[64]) {
    if(!cr_output(out,64,p,n)) return SYSCALL_EINVAL;
    crypto_sha512_ctx c; crypto_sha512_init(&c);
    int r=crypto_sha512_update(&c,p,n);
    if(!r) r=crypto_sha512_final(&c,out);
    crypto_wipe(&c,sizeof(c)); return r;
}
int crypto_hmac_sha256(const uint8_t *key,size_t kn,const uint8_t *msg,size_t n,uint8_t out[32]) {
    if(!cr_output(out,32,key,kn) || !cr_output(out,32,msg,n)) return SYSCALL_EINVAL;
    uint8_t pad[64]={0},inner[32]={0}; digest_t c;
    int r=0;
    if(kn>64) { r=crypto_sha256(key,kn,pad); if(r) goto done; }
    else if(kn) memcpy(pad,key,kn);
    for(unsigned i=0;i<64;i++) pad[i]^=0x36;
    digest_init(&c,DIGEST_SHA256); digest_update(&c,pad,64);
    if(!digest_update(&c,msg,n)) {r=SYSCALL_EINVAL;goto done;}
    digest_final(&c,inner);
    for(unsigned i=0;i<64;i++) pad[i]^=0x36^0x5c;
    digest_init(&c,DIGEST_SHA256); digest_update(&c,pad,64); digest_update(&c,inner,32);
    digest_final(&c,out);
done:
    crypto_wipe(pad,sizeof(pad)); crypto_wipe(inner,sizeof(inner)); crypto_wipe(&c,sizeof(c)); return r;
}

int crypto_hkdf_extract(const uint8_t *salt,size_t salt_len,const uint8_t *ikm,size_t ikm_len,uint8_t prk_out[32]) {
    if(!prk_out || !cr_range(prk_out,32)) return SYSCALL_EINVAL;
    if(salt_len > 0) {
        if(!salt || !cr_range(salt,salt_len)) return SYSCALL_EINVAL;
        if(cr_overlap(prk_out,32,salt,salt_len)) return SYSCALL_EINVAL;
    }
    if(ikm_len > 0) {
        if(!ikm || !cr_range(ikm,ikm_len)) return SYSCALL_EINVAL;
        if(cr_overlap(prk_out,32,ikm,ikm_len)) return SYSCALL_EINVAL;
    }
    uint8_t zero_salt[32] = {0};
    const uint8_t *s = salt;
    size_t slen = salt_len;
    if(salt_len == 0) {
        s = zero_salt;
        slen = sizeof(zero_salt);
    }
    int r = crypto_hmac_sha256(s,slen,ikm,ikm_len,prk_out);
    crypto_wipe(zero_salt,sizeof(zero_salt));
    if(r != 0) {
        crypto_wipe(prk_out,32);
        return r;
    }
    return 0;
}

int crypto_hkdf_expand(const uint8_t *prk,size_t prk_len,const uint8_t *info,size_t info_len,uint8_t *okm,size_t okm_len) {
    if(okm_len > CRYPTO_HKDF_MAX_OKM_LEN) return SYSCALL_EINVAL;
    if(!prk || prk_len < 32 || !cr_range(prk,prk_len)) return SYSCALL_EINVAL;
    if(info_len > 0 && (!info || !cr_range(info,info_len))) return SYSCALL_EINVAL;
    if(okm_len > 0) {
        if(!okm || !cr_range(okm,okm_len)) return SYSCALL_EINVAL;
        if(cr_overlap(okm,okm_len,prk,prk_len)) return SYSCALL_EINVAL;
        if(info_len > 0 && cr_overlap(okm,okm_len,info,info_len)) return SYSCALL_EINVAL;
    } else {
        return 0;
    }

    uint8_t k_pad[64] = {0};
    uint8_t ipad[64] = {0};
    uint8_t opad[64] = {0};
    uint8_t inner[32] = {0};
    uint8_t t[32] = {0};
    digest_t d;
    size_t t_len = 0;
    size_t written = 0;
    int r = 0;

    if(prk_len > 64) {
        r = crypto_sha256(prk,prk_len,k_pad);
        if(r) goto fail;
    } else {
        memcpy(k_pad,prk,prk_len);
    }

    for(unsigned i = 0; i < 64; i++) {
        ipad[i] = (uint8_t)(k_pad[i] ^ 0x36);
        opad[i] = (uint8_t)(k_pad[i] ^ 0x5c);
    }
    crypto_wipe(k_pad,sizeof(k_pad));

    size_t n = (okm_len + 31) / 32;
    for(unsigned i = 1; i <= n; i++) {
        uint8_t ctr = (uint8_t)i;
        digest_init(&d,DIGEST_SHA256);
        if(!digest_update(&d,ipad,sizeof(ipad)) ||
           (t_len > 0 && !digest_update(&d,t,t_len)) ||
           (info_len > 0 && !digest_update(&d,info,info_len)) ||
           !digest_update(&d,&ctr,1)) {
            goto fail;
        }
        digest_final(&d,inner);

        digest_init(&d,DIGEST_SHA256);
        if(!digest_update(&d,opad,sizeof(opad)) ||
           !digest_update(&d,inner,sizeof(inner))) {
            goto fail;
        }
        digest_final(&d,t);
        t_len = sizeof(t);

        size_t chunk = (okm_len - written < 32) ? (okm_len - written) : 32;
        memcpy(okm + written,t,chunk);
        written += chunk;
    }

    crypto_wipe(ipad,sizeof(ipad));
    crypto_wipe(opad,sizeof(opad));
    crypto_wipe(inner,sizeof(inner));
    crypto_wipe(t,sizeof(t));
    crypto_wipe(&d,sizeof(d));
    return 0;

fail:
    if(okm && okm_len > 0) crypto_wipe(okm,okm_len);
    crypto_wipe(k_pad,sizeof(k_pad));
    crypto_wipe(ipad,sizeof(ipad));
    crypto_wipe(opad,sizeof(opad));
    crypto_wipe(inner,sizeof(inner));
    crypto_wipe(t,sizeof(t));
    crypto_wipe(&d,sizeof(d));
    return SYSCALL_EINVAL;
}

int crypto_hkdf(const uint8_t *salt,size_t salt_len,const uint8_t *ikm,size_t ikm_len,const uint8_t *info,size_t info_len,uint8_t *okm,size_t okm_len) {
    if(okm_len > CRYPTO_HKDF_MAX_OKM_LEN) return SYSCALL_EINVAL;
    if(okm_len > 0) {
        if(!okm || !cr_range(okm,okm_len)) return SYSCALL_EINVAL;
        if(salt_len > 0 && cr_overlap(okm,okm_len,salt,salt_len)) return SYSCALL_EINVAL;
        if(ikm_len > 0 && cr_overlap(okm,okm_len,ikm,ikm_len)) return SYSCALL_EINVAL;
        if(info_len > 0 && cr_overlap(okm,okm_len,info,info_len)) return SYSCALL_EINVAL;
    }

    uint8_t prk[32] = {0};
    int r = crypto_hkdf_extract(salt,salt_len,ikm,ikm_len,prk);
    if(r != 0) {
        crypto_wipe(prk,sizeof(prk));
        if(okm && okm_len > 0) crypto_wipe(okm,okm_len);
        return r;
    }

    r = crypto_hkdf_expand(prk,sizeof(prk),info,info_len,okm,okm_len);
    crypto_wipe(prk,sizeof(prk));
    if(r != 0) {
        if(okm && okm_len > 0) crypto_wipe(okm,okm_len);
        return r;
    }
    return 0;
}

