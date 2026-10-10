#ifndef FORTRESS_PERMISSION_VALUES_H
#define FORTRESS_PERMISSION_VALUES_H

#include "vfs.h"

/* Phase 2 pure value engine used by authoritative filesystem adapters.
 * Inputs are coherent, caller-owned values. No node pointers, callbacks,
 * locks, allocation or I/O. Filesystem adapters must obtain these values and
 * decide under their authoritative exclusion before staging any mutation.
 * Dynamic write policy (taint/freeze) is checked by the adapter separately. */
bool permission_group_member(const creds_t *actor, uint32_t gid);
int permission_access_value(const vfs_metadata_t *value, unsigned mask,
                            const creds_t *actor);
int permission_delete_value(const vfs_metadata_t *dir,
                            const vfs_metadata_t *victim, const creds_t *actor);
int permission_create_value(const vfs_metadata_t *dir, vfs_node_type_t type,
                            uint32_t requested, const creds_t *actor,
                            vfs_create_attrs_t *out);
/* Metadata proposals only. Adapters must authorize and commit under their
 * existing exclusion/transaction; errors leave output untouched. */
int permission_chmod_value(const vfs_metadata_t *old, uint32_t requested,
                           const creds_t *actor, vfs_metadata_t *out);
int permission_chown_value(const vfs_metadata_t *old, creds_id_change_t uid,
                           creds_id_change_t gid, const creds_t *actor,
                           vfs_metadata_t *out);
uint16_t permission_content_mode(const vfs_metadata_t *old, const creds_t *actor);

#endif
