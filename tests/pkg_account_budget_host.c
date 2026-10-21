/* Actual account redo engine, modelled file ownership/space only.
 * No EXT4 metadata, journal, permissions, allocator or account-service proof. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "account_redo.h"
typedef struct { unsigned char bytes[ACCOUNT_FILE_MAX];size_t size;bool present; } object;
static object files[ACCOUNT_ROLE_COUNT];
static account_workspace_t work;
static size_t block_size,retained_blocks,retained_objects,peak_blocks,peak_objects,peak_bytes;
static unsigned writes;
static bool stop_retirement,pin_canonical;
static size_t blocks(size_t n) {return (n+block_size-1)/block_size;}
static void measure(void) {
    size_t b=retained_blocks,n=retained_objects,bytes=0;
    for(unsigned i=0;i<ACCOUNT_ROLE_COUNT;i++) if(files[i].present) {
        b+=blocks(files[i].size);n++;bytes+=files[i].size;
    }
    if(b>peak_blocks)peak_blocks=b;
    if(n>peak_objects)peak_objects=n;
    if(bytes>peak_bytes)peak_bytes=bytes;
}
static int read_role(void *context,unsigned role,void *out,size_t cap,size_t *n) {
    (void)context;assert(role<ACCOUNT_ROLE_COUNT);
    if(!files[role].present)return -2;
    if(files[role].size>cap)return -22;
    *n=files[role].size;memcpy(out,files[role].bytes,*n);return 0;
}
static int write_role(void *context,unsigned role,const void *data,size_t n) {
    (void)context;assert(role<ACCOUNT_ROLE_COUNT && n && n<=ACCOUNT_FILE_MAX);
    /* The actual adapter creates absent files; all callers here must agree. */
    assert(!files[role].present);
    files[role].present=true;files[role].size=n;memcpy(files[role].bytes,data,n);
    writes++;measure();return 0;
}
static int remove_role(void *context,unsigned role) {
    (void)context;assert(role<ACCOUNT_ROLE_COUNT);
    if(role==ACCOUNT_PENDING && stop_retirement)return -5;
    if(role<3 && files[role].present && pin_canonical) {
        retained_blocks+=blocks(files[role].size);retained_objects++;
    }
    files[role].present=false;measure();return 0;
}
static int rename_role(void *context,unsigned from,unsigned to) {
    (void)context;assert(from<ACCOUNT_ROLE_COUNT && to<ACCOUNT_ROLE_COUNT);
    assert(files[from].present && !files[to].present);
    files[to]=files[from];files[from].present=false;measure();return 0;
}
static int sync_roles(void *context) {(void)context;return 0;}
static int validate(void *context,const char *const p[3],const size_t n[3]) {
    (void)context;
    /* Synthetic maximum-byte records, not userdb-valid account databases. */
    for(unsigned i=0;i<3;i++)if(n[i]!=ACCOUNT_FILE_MAX || !p[i])return -22;
    return 0;
}
static const account_io_t io={NULL,read_role,write_role,remove_role,rename_role,sync_roles,validate};
static void proposed(unsigned generation) {
    for(unsigned i=0;i<3;i++) {
        memset(work.files[i],(int)(generation+i),ACCOUNT_FILE_MAX);
        work.lengths[i]=ACCOUNT_FILE_MAX;
    }
}
static void reset(size_t bs) {
    memset(files,0,sizeof(files));memset(&work,0,sizeof(work));block_size=bs;
    retained_blocks=retained_objects=peak_blocks=peak_objects=peak_bytes=writes=0;
    stop_retirement=pin_canonical=false;
    for(unsigned i=0;i<3;i++) {files[i].present=true;files[i].size=ACCOUNT_FILE_MAX;}
    measure();
}
int main(void) {
    for(size_t bs=1024;bs<=4096;bs*=2) {
        reset(bs);proposed(1);assert(!account_redo_update(&io,&work,1));
        assert(writes==7 && peak_objects==8 && peak_bytes==7*ACCOUNT_FILE_MAX+ACCOUNT_INTENT_SIZE);
        assert(peak_blocks==7*blocks(ACCOUNT_FILE_MAX)+blocks(ACCOUNT_INTENT_SIZE));
        printf("update bs=%zu writes=%u peak_objects=%zu peak_data_blocks=%zu peak_payload_bytes=%zu\n",
               bs,writes,peak_objects,peak_blocks,peak_bytes);
        reset(bs);stop_retirement=true;proposed(2);
        assert(account_redo_update(&io,&work,2)==-5 && files[ACCOUNT_PENDING].present);
        /* Already published canonical bytes do not suppress recovery writes. */
        for(unsigned attempt=0;attempt<3;attempt++) {
            writes=0;assert(account_redo_recover(&io,&work)==-5 && writes==3);
            assert(files[ACCOUNT_PENDING].present);
        }
        stop_retirement=false;writes=0;assert(!account_redo_recover(&io,&work) && writes==3);
        assert(!files[ACCOUNT_PENDING].present);
        writes=0;assert(!account_redo_recover(&io,&work) && !writes);
        printf("recovery bs=%zu pending_attempts=4 writes_per_attempt=3 no_pending_writes=0\n",bs);
        reset(bs);pin_canonical=true;
        for(unsigned generation=1;generation<=20;generation++) {
            proposed(generation);assert(!account_redo_update(&io,&work,generation));
        }
        assert(retained_objects==60 && retained_blocks==60*blocks(ACCOUNT_FILE_MAX));
        /* Retired generations accumulate during the three-file publication too. */
        assert(peak_objects==67 && peak_blocks==66*blocks(ACCOUNT_FILE_MAX)+1);
        printf("pins_model bs=%zu updates=20 retained_objects=%zu retained_data_blocks=%zu peak_objects=%zu peak_data_blocks=%zu\n",
               bs,retained_objects,retained_blocks,peak_objects,peak_blocks);
    }
    puts("PASS: actual redo with modelled data-space accounting; not EXT4/grant acceptance");
}
