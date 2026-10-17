#ifndef KERNEL_DMESG_H
#define KERNEL_DMESG_H

#include "types.h"
#include "syscall.h"   /* for DMESG_SIZE */

/* Standard syslog severity levels (RFC 5424) */
#define KLOG_EMERG   0  /* System is unusable */
#define KLOG_ALERT   1  /* Action must be taken immediately */
#define KLOG_CRIT    2  /* Critical conditions */
#define KLOG_ERR     3  /* Error conditions */
#define KLOG_WARNING 4  /* Warning conditions */
#define KLOG_NOTICE  5  /* Normal but significant condition */
#define KLOG_INFO    6  /* Informational */
#define KLOG_DEBUG   7  /* Debug-level messages */

#define DMESG_ENTRY_MAX_LEN 512

/* Ring buffer entry struct */
typedef struct {
    uint8_t  level;
    uint16_t len;
} dmesg_entry_t;

void dmesg_append(char c);
void dmesg_append_str(const char *s, size_t n);
void dmesg_append_entry(uint8_t level, const char *s, size_t n);
size_t dmesg_read(char *dst, size_t cap);

/* Kernel logging API */
void klog(uint8_t level, const char *str);
void klog_write(uint8_t level, const char *str, size_t len);

/* Logging macros */
#define pr_emerg(str)  klog(KLOG_EMERG, (str))
#define pr_alert(str)  klog(KLOG_ALERT, (str))
#define pr_crit(str)   klog(KLOG_CRIT, (str))
#define pr_err(str)    klog(KLOG_ERR, (str))
#define pr_warn(str)   klog(KLOG_WARNING, (str))
#define pr_notice(str) klog(KLOG_NOTICE, (str))
#define pr_info(str)   klog(KLOG_INFO, (str))
#define pr_debug(str)  klog(KLOG_DEBUG, (str))

#endif