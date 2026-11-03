#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint64_t clock_value,clock_hz;
#define BOOT_TIMING_HZ() clock_hz
#define BOOT_TIMING_READ() clock_value
#include "boot_timing.h"
static char output[16384];static size_t length;
void serial_puts(const char *s){size_t n=strlen(s);assert(length+n<sizeof(output));memcpy(output+length,s,n+1);length+=n;}
static void clear(void){length=0;output[0]=0;}
int main(void){
 boot_timing_t t;clock_hz=1000000;clock_value=100;
 boot_timing_begin(&t,"discovery");clock_value+=1234567;boot_timing_mark(&t,"validation");clock_value+=765433;boot_timing_mark(&t,NULL);
 assert(t.count==2&&t.total==2000000);boot_timing_emit(&t,"ROOT");
 assert(strstr(output,"ROOT discovery: 1234 ms")&&strstr(output,"ROOT validation: 765 ms")&&strstr(output,"ROOT total: 2000 ms"));
 clear();boot_timing_begin(&t,"one");for(unsigned i=0;i<BOOT_TIMING_SLOTS+4;i++){clock_value+=1000;boot_timing_mark(&t,"next");}boot_timing_mark(&t,NULL);assert(t.count==BOOT_TIMING_SLOTS&&t.omitted==5&&t.total==(BOOT_TIMING_SLOTS+4)*1000);boot_timing_emit(&t,"BOUND");assert(strstr(output,"truncated"));
 clear();clock_value=100;boot_timing_begin(&t,"bad");clock_value=99;boot_timing_mark(&t,NULL);boot_timing_emit(&t,"ROOT");assert(!t.valid&&strstr(output,"clock moved backwards")&&!strstr(output,"0 ms"));
 clear();clock_hz=0;boot_timing_begin(&t,"off");boot_timing_mark(&t,NULL);boot_timing_emit(&t,"OFF");assert(!length);
 clock_hz=UINT64_MAX;boot_timing_begin(&t,"off");assert(!t.hz);
 clear();clock_hz=999;clock_value=0;boot_timing_begin(&t,"huge");clock_value=(UINT64_MAX/1000)*999+998;boot_timing_mark(&t,NULL);boot_timing_emit(&t,"ROOT");assert(strstr(output,"overflow"));
 clear();clock_hz=1000;clock_value=100;boot_timing_begin_at(&t,"clock-ready",20);assert(t.start==20&&t.entries[0].ticks==80);clock_value=120;boot_timing_mark(&t,NULL);assert(t.total==100);boot_timing_emit(&t,"KERNEL");assert(strstr(output,"early-platform: 80 ms")&&strstr(output,"total: 100 ms"));
 puts("PASS boot timing: calibrated deltas/totals, bounded phases, unavailable/rollback clocks and overflow");return 0;
}
