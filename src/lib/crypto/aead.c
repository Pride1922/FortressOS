#include "internal.h"
static bool args(uint8_t *out,const uint8_t *in,size_t n,const uint8_t tag[16],const uint8_t key[32],const uint8_t nonce[12],const uint8_t *ad,size_t an,bool encrypt) {
    if(n>CRYPTO_MAX_AEAD_BYTES || !cr_range(out,n) || !cr_range(in,n) ||
       !cr_range(tag,16) || !cr_range(key,32) || !cr_range(nonce,12) || !cr_range(ad,an)) return false;
    if(out!=in && cr_overlap(out,n,in,n)) return false;
    if(cr_overlap(out,n,tag,16) || cr_overlap(out,n,key,32) ||
       cr_overlap(out,n,nonce,12) || cr_overlap(out,n,ad,an)) return false;
    if(encrypt && (cr_overlap(tag,16,in,n) || cr_overlap(tag,16,key,32) ||
       cr_overlap(tag,16,nonce,12) || cr_overlap(tag,16,ad,an))) return false;
    return true;
}
int crypto_aead_lock(uint8_t *out,uint8_t tag[16],const uint8_t key[32],const uint8_t nonce[12],const uint8_t *ad,size_t an,const uint8_t *in,size_t n) {
    if(!args(out,in,n,tag,key,nonce,ad,an,true)) return SYSCALL_EINVAL;
    mono_crypto_aead_ctx ctx;
    mono_crypto_aead_init_ietf(&ctx,key,nonce);
    mono_crypto_aead_write(&ctx,out,tag,ad,an,in,n);
    crypto_wipe(&ctx,sizeof(ctx)); return 0;
}
int crypto_aead_unlock(uint8_t *out,const uint8_t tag[16],const uint8_t key[32],const uint8_t nonce[12],const uint8_t *ad,size_t an,const uint8_t *in,size_t n) {
    if(!args(out,in,n,tag,key,nonce,ad,an,false)) return SYSCALL_EINVAL;
    mono_crypto_aead_ctx ctx;
    mono_crypto_aead_init_ietf(&ctx,key,nonce);
    int r=mono_crypto_aead_read(&ctx,out,tag,ad,an,in,n);
    crypto_wipe(&ctx,sizeof(ctx)); return r;
}
