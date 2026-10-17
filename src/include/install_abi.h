#ifndef FORTRESS_INSTALL_ABI_H
#define FORTRESS_INSTALL_ABI_H
#include "types.h"
#define SYS_INSTALLCTL 71
#define INSTALL_ENUM 0u
#define INSTALL_ACQUIRE 1u
#define INSTALL_READ 2u
#define INSTALL_WRITE 3u
#define INSTALL_FLUSH 4u
#define INSTALL_RELEASE 5u
#define INSTALL_DEPLOY 6u
#define INSTALL_LOG 7u
#define INSTALL_LOG_START 0u
#define INSTALL_LOG_CANCELLED 1u
#define INSTALL_LOG_FAILED 2u
#define INSTALL_STATUS_USB_LOG_WARNING 1u
#define INSTALL_STATUS_ROOT_LOG_WARNING 2u
#define INSTALL_STATUS_HOSTKEY_WARNING 4u
/* Fixed events only: no console input, password or hash can be supplied. */
typedef struct {
    uint32_t size,version,event,stage;
    int32_t error;
    uint32_t reserved;
    int64_t call;
} install_log_event_v1_t;
_Static_assert(sizeof(install_log_event_v1_t)==32,"installer log event v1");
#define INSTALL_DRY_RUN 1u
#define INSTALL_CONFIRM_WHOLE_DISK 1u
#define INSTALL_ID_ELIGIBLE 1u
#define INSTALL_ID_SOURCE 2u
#define INSTALL_ID_MOUNTED 4u
/* INSTALL_ENUM output only: identity.reserved2[0..3] carries a read-only summary of the
 * existing partition table, gathered once at boot. It is not part of the digest and is
 * zero in every other identity. [0]=valid, [1]=scheme, [2]=partition count, [3]=kinds. */
#define INSTALL_SUMMARY_VALID 1u
#define INSTALL_SUMMARY_GPT 1u
#define INSTALL_SUMMARY_MBR 2u
#define INSTALL_SUMMARY_EFI 1u
#define INSTALL_SUMMARY_WINDOWS 2u
#define INSTALL_SUMMARY_LINUX 4u
#define INSTALL_SUMMARY_OTHER 8u
#define INSTALL_SUMMARY_UNREADABLE 16u
typedef struct {
    uint32_t size,version;
    uint64_t generation,sector_count;
    uint32_t sector_size,nsid;
    uint16_t segment;
    uint8_t bus,device,function,flags;
    uint16_t reserved0;
    char serial[20],model[40];
    uint8_t reserved1[4],nguid[16],eui64[8],digest[32],reserved2[32];
} install_identity_v1_t;
typedef struct {
    uint32_t size,version;
    uint64_t generation;
    uint32_t index,flags;
    uint8_t digest[32],reserved[8];
} install_target_v1_t;
typedef struct {
    uint32_t size,version;
    int32_t fd;
    uint32_t reserved0;
    uint64_t lba;
    uint32_t count,reserved1;
    uint64_t buffer;
} install_io_v1_t;
_Static_assert(sizeof(install_identity_v1_t)==192,"installer identity v1");
_Static_assert(offsetof(install_identity_v1_t,digest)==128,"installer identity hash offset");
_Static_assert(sizeof(install_target_v1_t)==64,"installer target v1");
_Static_assert(sizeof(install_io_v1_t)==40,"installer I/O v1");
/* Hash-only bounded deployment request; never carries a plaintext password.
 * GUIDs are GPT little-endian; install_id is 16 arbitrary nonzero seed bytes. */
typedef struct {
    uint32_t size,version;
    int32_t fd;
    uint32_t flags;
    uint8_t disk_guid[16],partition_guid[3][16],install_id[16];
    char operator_hash[78],recovery_hash[78];
    uint8_t reserved[4];
} install_deploy_v1_t;
/* Version 2 keeps the complete v1 prefix and adds the chosen login name. */
typedef struct {
    install_deploy_v1_t base;
    char username[32];
} install_deploy_v2_t;
_Static_assert(sizeof(install_deploy_v2_t)==288,"installer deployment v2");
/* Version 3 preserves v2 and adds a bounded DNS-label computer name. */
typedef struct { install_deploy_v1_t base; char username[32],hostname[64]; } install_deploy_v3_t;
_Static_assert(sizeof(install_deploy_v3_t)==352,"installer deployment v3");
/* v4 preserves v3. Optional target-generated opaque OpenSSH host container.
 * Buffer is private userspace input, copied/wiped by the runtime; never logged
 * or included in payload templates. Zero pointer/length requests no host key.
 * Provisioning does not enable SSH. Existing v1-v3 callers remain supported. */
typedef struct {
    install_deploy_v1_t base;char username[32],hostname[64];
    uint64_t hostkey;uint32_t hostkey_bytes,reserved;
} install_deploy_v4_t;
_Static_assert(sizeof(install_deploy_v4_t)==368,"installer deployment v4");
static inline bool install_hostname_valid(const char *name) {
    if (!name) return false;
    unsigned n=0;
    for (;n<64 && name[n];n++) {
        char c=name[n];
        if (!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || (c=='-' && n))) return false;
    }
    return n && n<64 && name[n-1]!='-';
}
static inline bool install_username_valid(const char name[32]) {
    unsigned n=0;
    for (;n<32 && name[n];n++) {
        char c=name[n];
        if (!((c>='a' && c<='z') || (c>='0' && c<='9' && n) || c=='_' || (c=='-' && n))) return false;
    }
    if (!n || n==32) return false;
    if (n==4 && name[0]=='r' && name[1]=='o' && name[2]=='o' && name[3]=='t') return false;
    if (n==5 && name[0]=='w' && name[1]=='h' && name[2]=='e' && name[3]=='e' && name[4]=='l') return false;
    return true;
}
typedef struct {
    uint32_t size,version,stage;
    int32_t error;
    uint64_t copied_bytes;
    /* reserved is the log-warning bitmask in current installer profiles. */
    uint32_t complete,reserved;
} install_status_v1_t;
_Static_assert(sizeof(install_deploy_v1_t)==256,"installer deployment v1");
_Static_assert(sizeof(install_status_v1_t)==32,"installer status v1");
#endif
