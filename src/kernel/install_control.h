#ifndef FORTRESS_INSTALL_CONTROL_H
#define FORTRESS_INSTALL_CONTROL_H
#include "install_abi.h"
#include "creds.h"
/* Boot-only registration; callbacks are bounded synchronous unlocked adapters.
 * identity revalidates complete inventory, generation and source/mount exclusion
 * before every I/O. VFS errors; no normal live writable backend. */
typedef struct {
    void *context;
    int (*identity)(void *,install_identity_v1_t *);
    int (*read)(void *,uint64_t,uint32_t,void *);
    int (*write)(void *,uint64_t,uint32_t,const void *);
    int (*flush)(void *);
    /* Called while the control gate excludes all other lease/raw operations.
     * Trusted adapter owns detached mount handoff until freeze/discard. */
    int (*deploy)(void *,const install_deploy_v1_t *,install_status_v1_t *);
    void (*progress)(void *,unsigned);
} install_backend_t;
void install_control_boot(const install_backend_t *,bool installer_mode);
void install_identity_digest(install_identity_v1_t *);
bool install_control_owned(void);
int64_t install_control(unsigned,uintptr_t,size_t,uintptr_t,size_t,const creds_t *);
#endif
