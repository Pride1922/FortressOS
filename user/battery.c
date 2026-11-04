#include "types.h"
#include "syscall_abi.h"

__asm__(
    ".section .text\n"
    ".global _start\n"
    "_start:\n"
    "    andq $-16, %rsp\n"
    "    xorl %ebp, %ebp\n"
    "    call battery_main\n"
    "    movl %eax, %edi\n"
    "    xorl %eax, %eax\n"
    "    syscall\n"
    "    ud2\n"
    ".previous\n"
);

static long sys_write(int fd, const void *buf, size_t count) {
    long nr = SYS_WRITE;
    __asm__ volatile("syscall" : "+a"(nr) : "D"((uintptr_t)fd), "S"((uintptr_t)buf), "d"((uintptr_t)count)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}

static long sys_battery_get(battery_status_t *st) {
    long nr = SYS_BATTERY_GET;
    __asm__ volatile("syscall" : "+a"(nr) : "D"((uintptr_t)st)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}

static void print_str(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    sys_write(1, s, len);
}

static void print_u32(uint32_t val) {
    char buf[16];
    int idx = 0;
    if (val == 0) {
        buf[idx++] = '0';
    } else {
        char temp[16];
        int t = 0;
        while (val > 0) {
            temp[t++] = '0' + (val % 10);
            val /= 10;
        }
        while (t > 0) {
            buf[idx++] = temp[--t];
        }
    }
    sys_write(1, buf, idx);
}

int battery_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    battery_status_t st;
    long rc = sys_battery_get(&st);
    if (rc < 0) {
        if (rc == SYSCALL_EOPNOTSUPP)
            print_str("Battery telemetry unavailable: ACPI method execution is not supported\n");
        else if (rc == SYSCALL_ENODEV)
            print_str("No battery detected\n");
        else
            print_str("Battery telemetry read failed\n");
        return 1;
    }

    const char *state_str = "Unknown";
    if (st.state == BATTERY_STATE_DISCHARGING) {
        state_str = "Discharging";
    } else if (st.state == BATTERY_STATE_CHARGING) {
        state_str = "Charging";
    } else if (st.state == BATTERY_STATE_FULL) {
        state_str = "Full";
    }

    print_str("Battery: ");
    print_u32(st.capacity_percent);
    print_str("% (");
    print_str(state_str);
    print_str(")\n");

    if (st.time_remaining_mins > 0) {
        uint32_t hrs = st.time_remaining_mins / 60;
        uint32_t mins = st.time_remaining_mins % 60;
        print_str("Time remaining: ");
        print_u32(hrs);
        print_str("h ");
        if (mins < 10) print_str("0");
        print_u32(mins);
        print_str("m\n");
    }

    if (st.ac_online == AC_POWER_ONLINE) {
        print_str("Power: AC Connected\n");
    } else if (st.ac_online == AC_POWER_OFFLINE) {
        print_str("Power: On Battery\n");
    }

    if (st.voltage_mv > 0) {
        print_str("Voltage: ");
        print_u32(st.voltage_mv / 1000);
        print_str(".");
        uint32_t mv_rem = st.voltage_mv % 1000;
        if (mv_rem < 100) print_str("0");
        if (mv_rem < 10) print_str("0");
        print_u32(mv_rem);
        print_str(" V\n");
    }

    if (st.temperature_c > 0) {
        print_str("Thermal: ");
        print_u32((uint32_t)st.temperature_c);
        print_str(" C\n");
    }

    return 0;
}
