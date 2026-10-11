#include "account_redo.h"
#include "../../user/tools/digest.h"
#include "string.h"
#define A_EIO 5
#define A_ENOENT 2
#define A_EINVAL 22
static void hash(const void *p,size_t n,uint8_t out[32]) {
    digest_t d;digest_init(&d,DIGEST_SHA256);(void)digest_update(&d,p,n);digest_final(&d,out);
    volatile uint8_t *q=(void *)&d;for (size_t i=0;i<sizeof(d);i++) q[i]=0;
}
void account_redo_wipe(account_workspace_t *w) {
    volatile uint8_t *p=(void *)w;for (size_t i=0;i<sizeof(*w);i++) p[i]=0;
}
static bool api(const account_io_t *io,account_workspace_t *w) {
    return io && w && io->read && io->write && io->remove && io->rename && io->sync && io->validate;
}
static int validate(const account_io_t *io,account_workspace_t *w) {
    const char *p[3]={w->files[0],w->files[1],w->files[2]};
    for (unsigned i=0;i<3;i++) if (!w->lengths[i] || w->lengths[i]>ACCOUNT_FILE_MAX) return -A_EINVAL;
    return io->validate(io->context,p,w->lengths);
}
/* Binary little-endian record: magic[8], generation u64, three lengths u32,
 * reserved[4], three SHA256s, SHA256 of the preceding 128 bytes. Exact size;
 * no padding, paths, arbitrary filenames or unchecked host structs. */
static void record(account_workspace_t *w,uint64_t generation) {
    memset(w->intent,0,sizeof(w->intent));memcpy(w->intent,"FOSACCT1",8);
    for (unsigned j=0;j<8;j++) w->intent[8+j]=(uint8_t)(generation>>(j*8));
    for (unsigned i=0;i<3;i++) {
        for (unsigned j=0;j<4;j++) w->intent[16+i*4+j]=(uint8_t)(w->lengths[i]>>(j*8));
        hash(w->files[i],w->lengths[i],w->intent+32+i*32);
    }
    hash(w->intent,128,w->intent+128);
}
static bool parse(account_workspace_t *w) {
    uint8_t sum[32];hash(w->intent,128,sum);
    if (memcmp(w->intent,"FOSACCT1",8) || memcmp(sum,w->intent+128,32)) return false;
    bool generation=false;for (unsigned j=8;j<16;j++) generation|=w->intent[j]!=0;
    if (!generation) return false;
    for (unsigned j=28;j<32;j++) if (w->intent[j]) return false;
    for (unsigned i=0;i<3;i++) {
        uint32_t n=0;for (unsigned j=0;j<4;j++) n|=(uint32_t)w->intent[16+i*4+j]<<(j*8);
        if (!n || n>ACCOUNT_FILE_MAX) return false;
        w->lengths[i]=n;
    }
    return true;
}
static int cleanup(const account_io_t *io) {
    for (unsigned i=ACCOUNT_STAGE_PASSWD;i<=ACCOUNT_INTENT_TEMP;i++) {
        int r=io->remove(io->context,i);if (r) return r;
    }
    return io->sync(io->context);
}
static int publish(const account_io_t *io,account_workspace_t *w) {
    for (unsigned i=0;i<3;i++) {
        int r=io->remove(io->context,ACCOUNT_TEMP_PASSWD+i);if (r) return r;
        r=io->write(io->context,ACCOUNT_TEMP_PASSWD+i,w->files[i],w->lengths[i]);if (r) return r;
        r=io->sync(io->context);if (r) return r;
        r=io->remove(io->context,i);if (r) return r;
        r=io->rename(io->context,ACCOUNT_TEMP_PASSWD+i,i);if (r) return r;
        r=io->sync(io->context);if (r) return r;
    }
    /* Read back all canonical bytes, rather than treating syscall success as
     * an independent oracle. Immutable staging remains through final barrier. */
    for (unsigned i=0;i<3;i++) {
        size_t n=0;int r=io->read(io->context,i,w->files[i],ACCOUNT_FILE_MAX,&n);
        if (r) return r;
        uint8_t sum[32];hash(w->files[i],n,sum);
        if (n!=w->lengths[i] || memcmp(sum,w->intent+32+i*32,32)) return -A_EIO;
    }
    int r=validate(io,w);if (r) return r;
    r=io->sync(io->context);if (r) return r;
    r=io->remove(io->context,ACCOUNT_PENDING);if (r) return r;
    r=io->sync(io->context);if (r) return r;
    /* Cleanup is deliberately outside the completion point. Failure remains
     * an error to the caller; reboot accepts the verified new canonical set. */
    return cleanup(io);
}
int account_redo_recover(const account_io_t *io,account_workspace_t *w) {
    if (!api(io,w)) return -A_EINVAL;
    size_t n=0;int r=io->read(io->context,ACCOUNT_PENDING,w->intent,sizeof(w->intent),&n);
    if (r==-A_ENOENT) {
        for (unsigned i=0;i<3;i++) {
            r=io->read(io->context,i,w->files[i],ACCOUNT_FILE_MAX,&w->lengths[i]);if (r) return r;
        }
        r=validate(io,w);return r ? r : cleanup(io);
    }
    if (r) return r;
    if (n!=ACCOUNT_INTENT_SIZE || !parse(w)) return -A_EINVAL;
    for (unsigned i=0;i<3;i++) {
        r=io->read(io->context,ACCOUNT_STAGE_PASSWD+i,w->files[i],ACCOUNT_FILE_MAX,&n);if (r) return r;
        uint8_t sum[32];hash(w->files[i],n,sum);
        if (n!=w->lengths[i] || memcmp(sum,w->intent+32+i*32,32)) return -A_EINVAL;
    }
    r=validate(io,w);return r ? r : publish(io,w);
}
int account_redo_update(const account_io_t *io,account_workspace_t *w,uint64_t generation) {
    if (!api(io,w) || !generation) return -A_EINVAL;
    int r=validate(io,w);if (r) return r;
    /* Refuse to overwrite redo evidence from any earlier update. */
    uint8_t old[ACCOUNT_INTENT_SIZE];size_t n=0;
    r=io->read(io->context,ACCOUNT_PENDING,old,sizeof(old),&n);
    if (r!=-A_ENOENT) return r ? r : -A_EINVAL;
    r=cleanup(io);if (r) return r;
    record(w,generation);
    for (unsigned i=0;i<3;i++) {
        r=io->write(io->context,ACCOUNT_STAGE_PASSWD+i,w->files[i],w->lengths[i]);if (r) return r;
    }
    r=io->sync(io->context);if (r) return r;
    r=io->write(io->context,ACCOUNT_INTENT_TEMP,w->intent,sizeof(w->intent));if (r) return r;
    r=io->sync(io->context);if (r) return r;
    r=io->rename(io->context,ACCOUNT_INTENT_TEMP,ACCOUNT_PENDING);if (r) return r;
    r=io->sync(io->context);if (r) return r;
    return publish(io,w);
}
