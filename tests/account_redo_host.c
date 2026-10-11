/* Actual redo engine; labelled in-memory durability adapter. Each operation
 * may persist immediately, modeling synchronous journal commits; barriers can
 * additionally fail. This is not a physical sector-tear or EXT4 test. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "account_redo.h"
typedef struct { unsigned char data[ACCOUNT_FILE_MAX];size_t n;bool present;unsigned mode; } object;
typedef struct { object files[ACCOUNT_ROLE_COUNT];unsigned step,cut;bool after; } disk;
static disk initial,current;
static account_workspace_t work;
static const char *old[3]={"old passwd\n","old group\n","old shadow\n"};
static const char *next[3]={"new passwd\n","new group\n","new shadow\n"};
static bool fail(disk *d,bool after) {return d->cut && d->step==d->cut && d->after==after;}
static int read_file(void *p,unsigned role,void *out,size_t cap,size_t *n) {
    disk *d=p;object *o=&d->files[role];d->step++;
    if (fail(d,false)) return -5;
    if (!o->present) return fail(d,true) ? -5 : -2;
    if (o->n>cap) return -22;
    memcpy(out,o->data,o->n);*n=o->n;return fail(d,true) ? -5 : 0;
}
static int write_file(void *p,unsigned role,const void *in,size_t n) {
    disk *d=p;d->step++;if (fail(d,false)) return -28;
    object *o=&d->files[role];memcpy(o->data,in,n);o->n=n;o->present=true;
    o->mode=role>=ACCOUNT_STAGE_PASSWD && role<=ACCOUNT_STAGE_SHADOW ? 0600 :
        role==ACCOUNT_TEMP_SHADOW || role>=ACCOUNT_INTENT_TEMP ? 0600 : 0644;
    return fail(d,true) ? -5 : 0;
}
static int remove_file(void *p,unsigned role) {
    disk *d=p;d->step++;if (fail(d,false)) return -5;
    d->files[role].present=false;return fail(d,true) ? -5 : 0;
}
static int rename_file(void *p,unsigned from,unsigned to) {
    disk *d=p;d->step++;if (fail(d,false)) return -5;
    if (d->files[to].present || !d->files[from].present) return -17;
    d->files[to]=d->files[from];d->files[from].present=false;return fail(d,true) ? -5 : 0;
}
static int sync_disk(void *p) {disk *d=p;d->step++;return fail(d,false)||fail(d,true) ? -5 : 0;}
static int validate(void *p,const char *const files[3],const size_t lengths[3]) {
    (void)p;bool a=true,b=true;
    for (unsigned i=0;i<3;i++) {
        a &= lengths[i]==strlen(old[i]) && !memcmp(files[i],old[i],lengths[i]);
        b &= lengths[i]==strlen(next[i]) && !memcmp(files[i],next[i],lengths[i]);
    }
    return a||b ? 0 : -22;
}
static account_io_t io={&current,read_file,write_file,remove_file,rename_file,sync_disk,validate};
static void proposed(void) {
    memset(&work,0,sizeof(work));
    for (unsigned i=0;i<3;i++) {work.lengths[i]=strlen(next[i]);memcpy(work.files[i],next[i],work.lengths[i]);}
}
static bool is_set(const char *const set[3]) {
    for (unsigned i=0;i<3;i++) if (!current.files[i].present || current.files[i].n!=strlen(set[i]) ||
        memcmp(current.files[i].data,set[i],strlen(set[i]))) return false;
    return true;
}
int main(void) {
    for (unsigned i=0;i<3;i++) {
        object *o=&initial.files[i];o->present=true;o->n=strlen(old[i]);memcpy(o->data,old[i],o->n);o->mode=i==2 ? 0600 : 0644;
    }
    current=initial;proposed();assert(!account_redo_update(&io,&work,1));unsigned steps=current.step;
    assert(is_set(next));assert(current.files[ACCOUNT_SHADOW].mode==0600);
    unsigned cases=0;
    for (unsigned cut=1;cut<=steps;cut++) for (unsigned after=0;after<2;after++) {
        current=initial;current.cut=cut;current.after=after;proposed();
        int r=account_redo_update(&io,&work,2);assert(r<0);
        bool pending=current.files[ACCOUNT_PENDING].present;
        if (!pending) assert(is_set(old)||is_set(next));
        disk interrupted=current;current.cut=0;current.step=0;
        assert(!account_redo_recover(&io,&work));assert(is_set(pending ? next : is_set(next) ? next : old));
        assert(current.files[ACCOUNT_SHADOW].mode==0600);
        unsigned recovery_steps=current.step;
        for (unsigned restart=1;restart<=recovery_steps;restart++) {
            current=interrupted;current.cut=restart;current.after=true;current.step=0;
            (void)account_redo_recover(&io,&work);current.cut=0;current.step=0;
            assert(!account_redo_recover(&io,&work));assert(is_set(old)||is_set(next));cases++;
        }
        cases++;
    }
    /* Every byte of a durable intent is covered by strict parser/checksum. */
    current=initial;proposed();current.cut=25;current.after=true;
    (void)account_redo_update(&io,&work,3);
    /* Capture at the first canonical remove, independently of step numbering. */
    disk staged={0};bool found=false;
    for (unsigned cut=1;cut<=steps;cut++) {
        current=initial;current.cut=cut;current.after=true;proposed();(void)account_redo_update(&io,&work,3);
        if (current.files[ACCOUNT_PENDING].present) {staged=current;found=true;break;}
    }
    assert(found);
    for (unsigned i=0;i<ACCOUNT_INTENT_SIZE;i++) {
        current=staged;current.cut=0;current.files[ACCOUNT_PENDING].data[i]^=1;
        assert(account_redo_recover(&io,&work)<0);assert(current.files[ACCOUNT_PENDING].present);cases++;
    }
    current=staged;current.cut=0;current.files[ACCOUNT_STAGE_SHADOW].mode=0644;
    /* Mode rejection belongs to the filesystem adapter, not this byte engine. */
    current.files[ACCOUNT_STAGE_SHADOW].data[0]^=1;assert(account_redo_recover(&io,&work)<0);
    account_redo_wipe(&work);for (size_t i=0;i<sizeof(work);i++) assert(!((unsigned char *)&work)[i]);
    printf("account redo host PASS: %u failure/restart/corruption cases, %u operation boundaries\n",cases,steps);
}
