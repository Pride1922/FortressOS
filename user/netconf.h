#ifndef USER_NETCONF_H
#define USER_NETCONF_H

#include "types.h"

#define NETCONF_DEFAULT_PATH "/mnt/.fortress/network.conf"
#define NETCONF_MAX_FILE_SIZE 4096
#define NETCONF_MAX_LINE_LEN  256

typedef struct {
    uint32_t address;      /* be32 */
    uint8_t  prefix;       /* 1..30 */
    uint32_t netmask;      /* be32 */
    uint32_t gateway;      /* be32 (0 if none) */
    uint32_t dns;          /* be32 (0 if none) */
    bool has_address;
    bool has_gateway;
    bool has_dns;
} netconf_t;

/* Parse an IPv4 dotted-decimal string into network byte order. Returns 0 on success, -1 on error. */
int netconf_parse_ipv4(const char *str, uint32_t *out_ip);

/* Parse a CIDR IPv4 string (A.B.C.D/P) into IP, prefix (1..30), and netmask. Returns 0 on success, -1 on error. */
int netconf_parse_cidr(const char *str, uint32_t *out_ip, uint8_t *out_prefix, uint32_t *out_mask);

/* Parse a netmask dotted-decimal string into prefix and mask. Returns 0 on success, -1 on error. */
int netconf_parse_netmask(const char *str, uint8_t *out_prefix, uint32_t *out_mask);

/* Parse config file buffer.
 * tool_name: e.g. "ifup", "nslookup", "nc" (for warnings)
 * verbose_warnings: if true, print warnings for unknown/duplicate keys to stderr
 * err_msg: optional buffer for fatal error message
 * err_cap: size of err_msg
 * Returns 0 on success, -1 on fatal error.
 */
int netconf_parse_buffer(const char *buf, size_t len, const char *source_name,
                         const char *tool_name, bool verbose_warnings,
                         netconf_t *out_conf, char *err_msg, size_t err_cap);

/* Read and parse file at path using SYS_OPEN / SYS_READ / SYS_CLOSE. */
int netconf_parse_file(const char *path, const char *tool_name, bool verbose_warnings,
                       netconf_t *out_conf, char *err_msg, size_t err_cap);

/* Helper to read DNS server IPv4 from config file. Returns 0 if found and valid, -1 on error or missing. */
int netconf_read_dns(const char *path, uint32_t *out_dns);

#endif /* USER_NETCONF_H */
