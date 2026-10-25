/* Private copied translation unit only. */
static uint32_t px_created_inode;
static void *px_node_new(ext4_mount_t *);
static void px_node_drop(void *);
static void *px_publication_new(void);
static void px_publication_drop(void *);
static bool px_cache_room(ext4_mount_t *);
static bool px_orphan_room(ext4_engine_t *);
static bool px_orphan_pause(ext4_engine_t *);
static int e4_semantic_commit(ext4_engine_t *);
