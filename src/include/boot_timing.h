#ifndef FORTRESS_BOOT_TIMING_H
#define FORTRESS_BOOT_TIMING_H
#include "types.h"
#include "serial.h"
/* Boot-owner local observations only. No locks, allocation, media I/O or policy.
 * Host filesystem fixtures leave this disabled unless a clock is injected. */
#ifndef BOOT_TIMING_HZ
#if defined(HOST_TEST) || defined(__linux__)
#define BOOT_TIMING_HZ() 0ULL
#define BOOT_TIMING_READ() 0ULL
#else
#include "apic.h"
#define BOOT_TIMING_HZ() apic_poll_clock_hz()
#define BOOT_TIMING_READ() apic_poll_clock_read()
#endif
#endif
#define BOOT_TIMING_SLOTS 16u
typedef struct {
    uint64_t hz,start,last,total;
    const char *phase;
    unsigned count,omitted;
    bool valid;
    struct { const char *name;uint64_t ticks; } entries[BOOT_TIMING_SLOTS];
} boot_timing_t;
static inline void boot_timing_begin(boot_timing_t *t,const char *phase) {
    *t=(boot_timing_t){0};t->hz=BOOT_TIMING_HZ();
    if (!t->hz || t->hz>UINT64_MAX/1000) {t->hz=0;return;}
    t->start=t->last=BOOT_TIMING_READ();t->phase=phase;t->valid=true;
}
static inline void boot_timing_mark(boot_timing_t *t,const char *next) {
    if (!t->hz || !t->valid) return;
    uint64_t now=BOOT_TIMING_READ();
    if (now<t->last) {t->valid=false;return;}
    if (t->phase) {
        if (t->count<BOOT_TIMING_SLOTS) {
            t->entries[t->count].name=t->phase;
            t->entries[t->count++].ticks=now-t->last;
        } else t->omitted++;
    }
    t->last=now;t->phase=next;t->total=now-t->start;
}
static inline void boot_timing_text(char *out,unsigned cap,unsigned *n,const char *s) {
    while (*s && *n+1<cap) out[(*n)++]=*s++;
}
static inline void boot_timing_line(const boot_timing_t *t,const char *scope,const char *phase,uint64_t ticks) {
    char out[192],number[24];unsigned n=0,used=0;
    boot_timing_text(out,sizeof(out),&n,"[BOOT TIME] ");
    boot_timing_text(out,sizeof(out),&n,scope);boot_timing_text(out,sizeof(out),&n," ");
    boot_timing_text(out,sizeof(out),&n,phase);boot_timing_text(out,sizeof(out),&n,": ");
    uint64_t whole=ticks/t->hz,fraction=(ticks%t->hz)*1000/t->hz;
    if (whole>UINT64_MAX/1000 || fraction>UINT64_MAX-whole*1000) boot_timing_text(out,sizeof(out),&n,"unavailable (overflow)");
    else {
        uint64_t ms=whole*1000+fraction;
        do {number[used++]=(char)('0'+ms%10);ms/=10;} while (ms);
        while (used && n+1<sizeof(out)) out[n++]=number[--used];
        boot_timing_text(out,sizeof(out),&n," ms");
    }
    boot_timing_text(out,sizeof(out),&n,"\n");out[n]=0;serial_puts(out);
}
/* Call only after the measured work has released its locks. Nested scopes
 * overlap: ROOT includes EXT4-MOUNT, and KERNEL includes ROOT. Do not sum them. */
static inline void boot_timing_emit(const boot_timing_t *t,const char *scope) {
    if (!t->hz) return;
    if (!t->valid) {serial_puts("[BOOT TIME] unavailable (clock moved backwards)\n");return;}
    for (unsigned i=0;i<t->count;i++) boot_timing_line(t,scope,t->entries[i].name,t->entries[i].ticks);
    boot_timing_line(t,scope,"total",t->total);
    if (t->omitted) serial_puts("[BOOT TIME] phase list truncated; total includes all work\n");
}
#endif
