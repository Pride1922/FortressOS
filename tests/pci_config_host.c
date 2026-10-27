/* Real legacy PCI address/data sequences, forced concurrent port adapters. */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#define TEST_SMP_MEMORY
#include "pci.h"
static void host_outl(uint16_t,uint32_t);
static uint32_t host_inl(uint16_t);
static void host_outw(uint16_t,uint16_t);
#define outl host_outl
#define inl host_inl
#define outw host_outw
#include "../src/drivers/pci.c"

static uint32_t address,values[4][64];
static _Thread_local bool held;
uint64_t spin_lock_irqsave(spinlock_t *lock) {assert(!held);assert(!pthread_mutex_lock(&lock->mutex));held=true;return 0;}
void spin_unlock_irqrestore(spinlock_t *lock,uint64_t flags) {(void)flags;assert(held);held=false;assert(!pthread_mutex_unlock(&lock->mutex));}
static unsigned device(void) {unsigned d=(address>>11)&31;assert(d>=1 && d<=4);return d-1;}
static void host_outl(uint16_t port,uint32_t value) {
    assert(held);
    if (port==PCI_CONFIG_ADDRESS_PORT) {address=value;sched_yield();}
    else {assert(port==PCI_CONFIG_DATA_PORT);values[device()][(address&255)/4]=value;}
}
static uint32_t host_inl(uint16_t port) {assert(held && port==PCI_CONFIG_DATA_PORT);return values[device()][(address&255)/4];}
static void host_outw(uint16_t port,uint16_t value) {
    assert(held && (port==PCI_CONFIG_DATA_PORT || port==PCI_CONFIG_DATA_PORT+2));
    uint32_t *word=&values[device()][(address&255)/4];
    if (port==PCI_CONFIG_DATA_PORT) *word=(*word&0xffff0000u)|value;
    else *word&=~((uint32_t)value<<16); /* status RW1C */
}
/* ECAM is not selected in this fixture. */
uint64_t *vmm_get_kernel_pml4_virt(void) {assert(false);return NULL;}
bool vmm_is_mapped(uint64_t *r,uintptr_t v) {(void)r;(void)v;assert(false);return false;}
uintptr_t vmm_get_physical_address(uint64_t *r,uintptr_t v) {(void)r;(void)v;assert(false);return 0;}
int vmm_map_page(uint64_t *r,uintptr_t v,uintptr_t p,uint64_t f) {(void)r;(void)v;(void)p;(void)f;assert(false);return -1;}
void serial_puts(const char *s) {(void)s;}
void serial_print_hex(uint64_t n) {(void)n;}
static void *worker(void *arg) {
    unsigned id=(unsigned)(uintptr_t)arg;
    for (unsigned i=0;i<2000;i++) {
        uint32_t value=(id<<24)|i;
        pci_write_config32(0,0,(uint8_t)id,0,0x10,value);
        assert(pci_read_config32(0,0,(uint8_t)id,0,0x10)==value);
        pci_write_config16(0,0,(uint8_t)id,0,4,(uint16_t)i);
        assert(pci_read_config32(0,0,(uint8_t)id,0,4)==(0xa5a50000u|i));
    }
    return NULL;
}
int main(void) {
    pthread_t threads[4];
    for (unsigned i=0;i<4;i++) {values[i][1]=0xa5a50000;assert(!pthread_create(&threads[i],NULL,worker,(void *)(uintptr_t)(i+1)));}
    for (unsigned i=0;i<4;i++) assert(!pthread_join(threads[i],NULL));
    assert(pci_read_config32(1,0,0,0,0)==UINT32_MAX);
    puts("PASS actual PCI legacy ports: 4 threads, 32000 forced-interleaving transactions, no cross-device reads/writes, command writes preserve RW1C status");
    return 0;
}
