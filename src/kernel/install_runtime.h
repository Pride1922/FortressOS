#ifndef FORTRESS_INSTALL_RUNTIME_H
#define FORTRESS_INSTALL_RUNTIME_H
#include "boot_info.h"
#include "install_control.h"
/* Trusted synchronous adapter. Control gate/lease excludes other raw access;
 * backing modules are retained for boot lifetime. Never publishes a namespace. */
int install_runtime_deploy(const boot_info_t *,const install_backend_t *,
    const install_identity_v1_t *,const install_deploy_v1_t *,install_status_v1_t *);
#endif
