/* Phase-7 single-owner writer. No spinlock is held across block callbacks.
 * Filesystem transaction coverage and mount policy are Phase 8. */
struct jbd2_writer {
    jbd2_plan_t *io;
    uint32_t *map, head, sequence, end;
    uint8_t uuid[16];
    bool (*validate_home)(void *,uint32_t,const uint8_t *);
    void *context;
    jbd2_credits_t credits;
    jbd2_write_state_t state;
    unsigned metadata_count, data_count, revoke_count;
    j_image_t metadata[JBD2_WRITE_IMAGES_MAX], data[JBD2_WRITE_IMAGES_MAX];
    uint32_t revokes[JBD2_WRITE_IMAGES_MAX];
};
static void jw_discard(jbd2_writer_t *w) {
    for (unsigned i=0;i<w->metadata_count;i++) kfree(w->metadata[i].bytes);
    for (unsigned i=0;i<w->data_count;i++) kfree(w->data[i].bytes);
    w->metadata_count=w->data_count=w->revoke_count=0;
}
void jbd2_writer_close(jbd2_writer_t *w) {
    spin_debug_assert_unheld();if (!w) return;
    jw_discard(w);jbd2_release(w->io);kfree(w->map);kfree(w);
}
jbd2_write_state_t jbd2_writer_state(const jbd2_writer_t *w) {
    return w ? w->state : JBD2_WRITE_FAILED;
}
int jbd2_writer_open(block_dev_t *dev,const jbd2_source_t *s,bool admitted,jbd2_writer_t **out) {
    spin_debug_assert_unheld();if (out) *out=NULL;
    if (!out || !s || !dev) return -VFS_EINVAL;
    if (!admitted || !dev->write_sector || !dev->flush) return -VFS_EROFS;
    jbd2_plan_t *p=NULL;int r=jbd2_analyze(dev,s,&p,NULL);if (r) return r;
    /* Even a valid uncommitted tail must be retired by explicit recovery. */
    if (j_be(p->super+28)) { jbd2_release(p);return -VFS_ENOSPC; }
    uint32_t head=j_be(p->super+88);if (!head) head=p->first;
    if (head<p->first || head>=p->length) { jbd2_release(p);return -VFS_EIO; }
    jbd2_writer_t *w=kcalloc(1,sizeof(*w));if (!w) { jbd2_release(p);return -VFS_ENOMEM; }
    w->io=p;w->head=head;w->sequence=j_be(p->super+24);
    w->map=kmalloc(p->length*sizeof(uint32_t));if (!w->map) { jbd2_writer_close(w);return -VFS_ENOMEM; }
    memcpy(w->map,s->journal_map,p->length*sizeof(uint32_t));memcpy(w->uuid,s->uuid,16);
    w->validate_home=s->validate_home;w->context=s->context;w->state=JBD2_WRITE_IDLE;
    *out=w;return 0;
}
static unsigned jw_blocks(jbd2_writer_t *w,unsigned images,unsigned revokes) {
    unsigned tags=(w->io->bs-32)/16;
    return images+(images ? (images+tags-1)/tags : 0)+(revokes ? 1 : 0)+1;
}
static int jw_staging(jbd2_writer_t *w) {
    if (!w) return -VFS_EINVAL;
    if (w->state==JBD2_WRITE_FAILED) return -VFS_EIO;
    return w->state==JBD2_WRITE_STAGING ? 0 : -VFS_EINVAL;
}
int jbd2_writer_begin(jbd2_writer_t *w,jbd2_credits_t c) {
    spin_debug_assert_unheld();if (!w) return -VFS_EINVAL;
    if (w->state==JBD2_WRITE_FAILED) return -VFS_EIO;
    if (w->state!=JBD2_WRITE_IDLE) return -VFS_EAGAIN;
    if (c.metadata>64 || c.ordered_data>64 || c.revokes>64) return -VFS_EFBIG;
    if (!c.metadata && !c.revokes) return -VFS_EINVAL;
    if (jw_blocks(w,c.metadata,c.revokes)>w->io->length-w->io->first) return -VFS_ENOSPC;
    w->credits=c;w->state=JBD2_WRITE_STAGING;return 0;
}
static int jw_stage(jbd2_writer_t *w,uint32_t block,const void *bytes,bool data) {
    int r=jw_staging(w);if (r) return r;
    if (!bytes || !j_target(w->io,block)) return -VFS_EINVAL;
    j_image_t *own=data ? w->data : w->metadata,*other=data ? w->metadata : w->data;
    unsigned *count=data ? &w->data_count : &w->metadata_count;
    unsigned other_count=data ? w->metadata_count : w->data_count;
    for (unsigned i=0;i<other_count;i++) if (other[i].block==block) return -VFS_EINVAL;
    for (unsigned i=0;i<w->revoke_count;i++) if (w->revokes[i]==block) return -VFS_EINVAL;
    if (!data && w->validate_home && !w->validate_home(w->context,block,bytes)) return -VFS_EIO;
    for (unsigned i=0;i<*count;i++) if (own[i].block==block) {
        memcpy(own[i].bytes,bytes,w->io->bs);return 0;
    }
    if (*count==(data ? w->credits.ordered_data : w->credits.metadata)) return -VFS_EFBIG;
    uint8_t *copy=kmalloc(w->io->bs);if (!copy) return -VFS_ENOMEM;
    memcpy(copy,bytes,w->io->bs);own[*count]=(j_image_t){block,0,copy};(*count)++;return 0;
}
int jbd2_writer_metadata(jbd2_writer_t *w,uint32_t b,const void *v) {
    spin_debug_assert_unheld();return jw_stage(w,b,v,false);
}
int jbd2_writer_data(jbd2_writer_t *w,uint32_t b,const void *v) {
    spin_debug_assert_unheld();return jw_stage(w,b,v,true);
}
int jbd2_writer_revoke(jbd2_writer_t *w,uint32_t block) {
    spin_debug_assert_unheld();int r=jw_staging(w);if (r) return r;
    if (!j_target(w->io,block)) return -VFS_EINVAL;
    for (unsigned i=0;i<w->metadata_count;i++) if (w->metadata[i].block==block) return -VFS_EINVAL;
    for (unsigned i=0;i<w->data_count;i++) if (w->data[i].block==block) return -VFS_EINVAL;
    for (unsigned i=0;i<w->revoke_count;i++) if (w->revokes[i]==block) return 0;
    if (w->revoke_count==w->credits.revokes) return -VFS_EFBIG;
    w->revokes[w->revoke_count++]=block;return 0;
}
int jbd2_writer_abort(jbd2_writer_t *w) {
    spin_debug_assert_unheld();int r=jw_staging(w);if (r) return r;
    jw_discard(w);w->state=JBD2_WRITE_IDLE;return 0;
}
static void jw_header(jbd2_writer_t *w,uint8_t *b,unsigned type) {
    memset(b,0,w->io->bs);j_put(b,J_MAGIC);j_put(b+4,type);j_put(b+8,w->sequence);
}
static void jw_seal(uint8_t *b,size_t size,unsigned offset,uint32_t seed) {
    j_put(b+offset,0);j_put(b+offset,j_crc(seed,b,size));
}
static bool jw_log(jbd2_writer_t *w,uint32_t *cursor,uint8_t *b) {
    if (!j_io(w->io,w->map[*cursor],b,true)) return false;
    *cursor=j_next(w->io,*cursor);return true;
}
int jbd2_writer_commit(jbd2_writer_t *w) {
    spin_debug_assert_unheld();int r=jw_staging(w);if (r) return r;
    if (!w->metadata_count && !w->revoke_count) return -VFS_EINVAL;
    jbd2_plan_t *p=w->io;uint8_t *b=p->scratch,*data=p->data,seq[4];j_put(seq,w->sequence);
    w->state=JBD2_WRITE_COMMITTING;
    for (unsigned i=0;i<w->data_count;i++) if (!j_io(p,w->data[i].block,w->data[i].bytes,true)) goto fail;
    if (w->data_count && !block_flush(p->dev)) goto fail;
    uint32_t cursor=w->head;unsigned capacity=(p->bs-32)/16;
    for (unsigned first=0;first<w->metadata_count;) {
        unsigned count=w->metadata_count-first;if (count>capacity) count=capacity;
        jw_header(w,b,1);unsigned at=12;
        for (unsigned n=0;n<count;n++) {
            j_image_t *image=&w->metadata[first+n];memcpy(data,image->bytes,p->bs);
            unsigned flags=(n ? 2u : 0u)|(n+1==count ? 8u : 0u);
            if (j_be(data)==J_MAGIC) { j_put(data,0);flags|=1; }
            j_put(b+at,image->block);j_put(b+at+4,flags);
            j_put(b+at+12,j_crc(j_crc(p->seed,seq,4),data,p->bs));at+=16;
            if (!n) { memcpy(b+at,w->uuid,16);at+=16; }
        }
        jw_seal(b,p->bs,p->bs-4,p->seed);if (!jw_log(w,&cursor,b)) goto fail;
        for (unsigned n=0;n<count;n++) {
            memcpy(data,w->metadata[first+n].bytes,p->bs);if (j_be(data)==J_MAGIC) j_put(data,0);
            if (!jw_log(w,&cursor,data)) goto fail;
        }
        first+=count;
    }
    if (w->revoke_count) {
        jw_header(w,b,5);j_put(b+12,16+w->revoke_count*4);
        for (unsigned n=0;n<w->revoke_count;n++) j_put(b+16+n*4,w->revokes[n]);
        jw_seal(b,p->bs,p->bs-4,p->seed);if (!jw_log(w,&cursor,b)) goto fail;
    }
    /* Retire a possibly stale commit before publishing a recoverable tail.
     * A cut before activation leaves the old empty journal; after activation
     * all descriptor/data/revoke bytes and the zero commit are durable. */
    memset(b,0,p->bs);if (!j_io(p,w->map[cursor],b,true) || !block_flush(p->dev)) goto fail;
    j_put(p->super+24,w->sequence);j_put(p->super+28,w->head);
    jw_seal(p->super,1024,252,UINT32_MAX);
    if (!j_io(p,p->super_block,p->super,true) || !block_flush(p->dev)) goto fail;
    jw_header(w,b,2);jw_seal(b,p->bs,16,p->seed);
    if (!jw_log(w,&cursor,b) || !block_flush(p->dev)) goto fail;
    w->end=cursor;w->state=JBD2_WRITE_DURABLE;return 0;
fail:
    w->state=JBD2_WRITE_FAILED;return -VFS_EIO;
}
int jbd2_writer_checkpoint(jbd2_writer_t *w) {
    spin_debug_assert_unheld();if (!w) return -VFS_EINVAL;
    if (w->state==JBD2_WRITE_FAILED) return -VFS_EIO;
    if (w->state!=JBD2_WRITE_DURABLE) return -VFS_EINVAL;
    jbd2_plan_t *p=w->io;w->state=JBD2_WRITE_CHECKPOINTING;
    for (unsigned i=0;i<w->metadata_count;i++)
        if (!j_io(p,w->metadata[i].block,w->metadata[i].bytes,true)) goto fail;
    if (!block_flush(p->dev)) goto fail;
    j_put(p->super+24,w->sequence+1);j_put(p->super+28,0);j_put(p->super+88,w->end);
    jw_seal(p->super,1024,252,UINT32_MAX);
    if (!j_io(p,p->super_block,p->super,true) || !block_flush(p->dev)) goto fail;
    w->head=w->end;w->sequence++;jw_discard(w);w->state=JBD2_WRITE_IDLE;return 0;
fail:
    w->state=JBD2_WRITE_FAILED;return -VFS_EIO;
}
