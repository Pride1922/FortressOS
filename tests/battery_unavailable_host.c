/* Compile the production driver without HOST_TEST: fixture branches must not
 * manufacture hardware telemetry. No ACPI/EC port access is expected. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "acpi_battery.h"
#include "acpi.h"
static unsigned logs, timers, ec_reads, table_reads;
static const char *message;
/* An SSDT containing battery presence and literal adapter/temperature returns,
 * but no executable battery status provider. The old production path invents
 * 100%/Full from this input. No host privileged port instructions may execute. */
static struct {
    acpi_sdt_header_t header;
    uint8_t body[32];
} firmware = {
    .header={.signature={'S','S','D','T'},.length=sizeof(acpi_sdt_header_t)+32},
    .body={'P','N','P','0','C','0','A',0,'_','P','S','R',0xa4,1,
           '_','T','M','P',0xa4,0x0b,0xa6,0x0b}
};
void serial_puts(const char *s) { message=s; ++logs; }
void serial_print_dec(uint64_t val) { (void)val; }
void serial_print_hex(uint64_t val) { (void)val; }
uint64_t apic_timer_get_ticks(void) { ++timers; return 0; }
uint64_t apic_timer_get_frequency(void) { ++timers; return 1000; }
acpi_sdt_header_t *acpi_find_table(const char *s) { (void)s; ++table_reads; return NULL; }
const acpi_sdt_header_t *acpi_get_dsdt(void) { return NULL; }
static size_t s_mock_table_count = 1;
size_t acpi_get_table_count(void) { ++table_reads; return s_mock_table_count; }
acpi_sdt_header_t *acpi_get_table_by_index(size_t i) { ++table_reads; return (s_mock_table_count && !i) ? &firmware.header : NULL; }
uintptr_t acpi_get_hhdm_offset(void) { return 0; }
bool acpi_ensure_mapped(uintptr_t p,size_t n) { (void)p; (void)n; return true; }
static int unavailable_ec(uint8_t reg,uint8_t *value) { (void)reg; (void)value; ++ec_reads; return -1; }
int main(void) {
    acpi_battery_reset();
    acpi_battery_set_ec_reader(unavailable_ec);
    battery_status_t sample, before;
    memset(&sample,0xa5,sizeof(sample)); before=sample;
    for (unsigned i=0;i<100;++i) {
        int rc=acpi_battery_get(&sample);
        if (rc!=SYSCALL_EIO)
            fprintf(stderr,"Unsupported fixture published rc=%d percent=%u state=%u AC=%u temperature=%d\n",
                    rc,sample.capacity_percent,sample.state,sample.ac_online,sample.temperature_c);
        assert(rc==SYSCALL_EIO);
        assert(!memcmp(&sample,&before,sizeof(sample)));
    }
    assert(acpi_battery_get(NULL)==SYSCALL_EFAULT);
    assert(logs>=1 && strstr(message,"Control Method Battery"));
    acpi_battery_force_update();
    assert(acpi_battery_get(&sample)==SYSCALL_EIO);
    assert(!memcmp(&sample,&before,sizeof(sample)));

    /* Desktop PC test: No battery device in ACPI tables -> ENODEV */
    s_mock_table_count = 0;
    acpi_battery_reset();
    assert(acpi_battery_get(&sample) == SYSCALL_ENODEV);
    assert(!memcmp(&sample, &before, sizeof(sample)));
    assert(!acpi_battery_is_present());

    puts("Production battery PASS: unreadable telemetry returns EIO, desktop PC returns ENODEV, caller output untouched, no fabricated samples");
}
