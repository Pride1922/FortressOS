#ifndef USER_WATCHTOWER_CONFIG_H
#define USER_WATCHTOWER_CONFIG_H

#include "types.h"
#include "watchtower.h"

#define WATCHTOWER_INSTALLED_SERVICE_DIR "/etc/watchtower"
#define WATCHTOWER_LIVE_SERVICE_DIR      "/mnt/.fortress/watchtower"
#define WATCHTOWER_MAX_CONFIG_SIZE       4096
#define WATCHTOWER_MAX_LINE_LEN          256

/* Parse a single service configuration from a memory buffer.
 * Returns 0 on success, negative error code on failure. */
int watchtower_config_parse_service(const char *buf, size_t len, const char *svc_name,
                                    service_t *out_svc, char *err_msg, size_t err_cap);

/* Load a service configuration from path.
 * Returns 0 on success, negative error code on failure. */
int watchtower_config_load_file(const char *path, const char *svc_name,
                                service_t *out_svc, char *err_msg, size_t err_cap);

/* Load a service with precedence: installed_path first, fallback to live_path.
 * Returns 0 on success, negative error code on failure. */
int watchtower_config_load_service(const char *installed_path, const char *live_path,
                                   const char *svc_name, service_t *out_svc,
                                   char *err_msg, size_t err_cap);

#endif /* USER_WATCHTOWER_CONFIG_H */
