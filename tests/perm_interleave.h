/* Actual VFS barrier before authoritative exclusion; host tests only. */
static pthread_mutex_t admission_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t admission_condition=PTHREAD_COND_INITIALIZER;
static unsigned admission_operation;
static bool admission_arrived,admission_released;
static int admission_result;
static const char *admission_path;
static const creds_t *admission_actor;
void vfs_admission_interleave_test(const vfs_node_t *node,unsigned op) {
    if (op!=admission_operation) return;
    assert(!vfs_permission(node,VFS_MAY_WRITE|VFS_MAY_EXEC,admission_actor));
    pthread_mutex_lock(&admission_mutex);admission_arrived=true;
    pthread_cond_broadcast(&admission_condition);
    while (!admission_released) pthread_cond_wait(&admission_condition,&admission_mutex);
    pthread_mutex_unlock(&admission_mutex);
}
static void *admission_worker(void *unused) {
    (void)unused;
    admission_result=admission_operation==1 ? vfs_mkdir_creds(admission_path,0700,admission_actor) :
        vfs_unlink_creds(admission_path,admission_actor);
    return NULL;
}
static pthread_t admission_start(unsigned op,const char *path,const creds_t *actor) {
    admission_operation=op;admission_path=path;admission_actor=actor;
    admission_arrived=admission_released=false;pthread_t worker;
    assert(!pthread_create(&worker,NULL,admission_worker,NULL));
    pthread_mutex_lock(&admission_mutex);
    while (!admission_arrived) pthread_cond_wait(&admission_condition,&admission_mutex);
    pthread_mutex_unlock(&admission_mutex);return worker;
}
static int admission_finish(pthread_t worker) {
    pthread_mutex_lock(&admission_mutex);admission_released=true;
    pthread_cond_broadcast(&admission_condition);pthread_mutex_unlock(&admission_mutex);
    assert(!pthread_join(worker,NULL));admission_operation=0;return admission_result;
}
