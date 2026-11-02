/* Compile the production driver without HOST_TEST: fixture branches must not
 * manufacture hardware telemetry. No ACPI/EC port access is expected. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "acpi_battery.h"
static unsigned logs;
void serial_puts(const char *s) { assert(strstr(s,"Telemetry unavailable")); ++logs; }
uint64_t apic_timer_get_ticks(void) { assert(!"unsupported reads must not poll"); return 0; }
uint64_t apic_timer_get_frequency(void) { assert(!"unsupported reads must not poll"); return 1000; }
int main(void) {
    acpi_battery_reset();
    battery_status_t sample, before;
    memset(&sample,0xa5,sizeof(sample)); before=sample;
    assert(acpi_battery_get(NULL)==SYSCALL_EFAULT);
    for (unsigned i=0;i<100;++i) {
        assert(acpi_battery_get(&sample)==SYSCALL_EOPNOTSUPP);
        assert(!memcmp(&sample,&before,sizeof(sample)));
    }
    assert(logs==1);
    acpi_battery_force_update();
    assert(acpi_battery_get(&sample)==SYSCALL_EOPNOTSUPP);
    assert(!memcmp(&sample,&before,sizeof(sample)));
    puts("Production battery PASS: unsupported telemetry is explicit, caller output untouched, no EC/timer polling or fabricated samples");
}
