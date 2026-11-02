#include "acpi_battery.h"
#include "serial.h"

#ifndef HOST_TEST
#include "acpi.h"
#include "apic.h"
#include "string.h"
#else
#include <string.h>
#include <stdio.h>
#endif

/* State Variables */
static battery_status_t g_cached_battery;
static uint64_t g_last_update_ticks = 0;
static bool g_has_cached = false;
static bool g_battery_present = false;
static bool g_battery_initialized = false;
static acpi_ec_read_fn_t g_ec_reader = NULL;
static int32_t g_mock_temp_c = 0;
static uint32_t g_mock_ac_online = AC_POWER_UNKNOWN;

/* Mock timer ticks for host tests */
#ifdef HOST_TEST
static uint64_t s_mock_ticks = 0;
static uint64_t s_mock_freq = 1000;
void test_set_mock_ticks(uint64_t ticks) { s_mock_ticks = ticks; }
void test_add_mock_ticks(uint64_t delta) { s_mock_ticks += delta; }
static inline uint64_t get_current_ticks(void) { return s_mock_ticks; }
static inline uint64_t get_tick_frequency(void) { return s_mock_freq; }
#else
static inline uint64_t get_current_ticks(void) { return apic_timer_get_ticks(); }
static inline uint64_t get_tick_frequency(void) {
    uint64_t freq = apic_timer_get_frequency();
    return (freq > 0) ? freq : 1000;
}
#endif

void acpi_battery_set_ec_reader(acpi_ec_read_fn_t reader) {
    g_ec_reader = reader;
}

void acpi_thermal_set_mock(int32_t temp_c) {
    g_mock_temp_c = temp_c;
}

void acpi_ac_adapter_set_mock(uint32_t online) {
    g_mock_ac_online = online;
}

void acpi_battery_reset(void) {
    memset(&g_cached_battery, 0, sizeof(g_cached_battery));
    g_last_update_ticks = 0;
    g_has_cached = false;
    g_battery_present = false;
    g_battery_initialized = false;
    g_ec_reader = NULL;
    g_mock_temp_c = 0;
    g_mock_ac_online = AC_POWER_UNKNOWN;
}

void acpi_battery_force_update(void) {
    g_has_cached = false;
}

/* Standard EC Bus I/O with bounded polling timeouts (~1-2ms) */
int acpi_ec_read(uint8_t addr, uint8_t *val) {
    if (!val) return -1;
    if (g_ec_reader) {
        return g_ec_reader(addr, val);
    }
#ifndef HOST_TEST
    /* 1. Wait for IBF == 0 */
    for (uint32_t i = 0; i < 50000; i++) {
        if ((inb(ACPI_EC_STATUS_PORT) & ACPI_EC_FLAG_IBF) == 0) break;
        io_wait();
        if (i == 49999) return -1;
    }

    /* 2. Send RD_EC command */
    outb(ACPI_EC_CMD_PORT, ACPI_EC_CMD_RD);

    /* 3. Wait for IBF == 0 */
    for (uint32_t i = 0; i < 50000; i++) {
        if ((inb(ACPI_EC_STATUS_PORT) & ACPI_EC_FLAG_IBF) == 0) break;
        io_wait();
        if (i == 49999) return -1;
    }

    /* 4. Send EC register address */
    outb(ACPI_EC_DATA_PORT, addr);

    /* 5. Wait for OBF == 1 */
    for (uint32_t i = 0; i < 50000; i++) {
        if ((inb(ACPI_EC_STATUS_PORT) & ACPI_EC_FLAG_OBF) != 0) break;
        io_wait();
        if (i == 49999) return -1;
    }

    /* 6. Read returned data */
    *val = inb(ACPI_EC_DATA_PORT);
    return 0;
#else
    (void)addr;
    return -1;
#endif
}

/* Bounded AML PNP0C0A device search */
bool acpi_battery_locate(const uint8_t *aml, size_t len, size_t *out_offset) {
    if (!aml || len < 8) return false;

    /* Search for string "PNP0C0A" or EISA ID */
    for (size_t i = 0; i + 7 < len; i++) {
        if (memcmp(aml + i, "PNP0C0A", 7) == 0) {
            if (out_offset) *out_offset = i;
            return true;
        }
        /* EISA ID PNP0C0A patterns: little-endian / standard AML */
        if (i + 4 <= len) {
            if ((aml[i] == 0x41 && aml[i+1] == 0xD0 && aml[i+2] == 0x0C && aml[i+3] == 0x0A) ||
                (aml[i] == 0xD0 && aml[i+1] == 0x41 && aml[i+2] == 0x0C && aml[i+3] == 0x0A) ||
                (aml[i] == 0x0A && aml[i+1] == 0x0C && aml[i+2] == 0xD0 && aml[i+3] == 0x41)) {
                if (out_offset) *out_offset = i;
                return true;
            }
        }
    }
    return false;
}

/* Parse an AML Integer starting at *pptr */
static bool parse_aml_integer(const uint8_t **pptr, const uint8_t *end, uint32_t *out_val) {
    if (*pptr >= end) return false;
    uint8_t op = **pptr;
    (*pptr)++;

    if (op == 0x00) { /* ZeroOp */
        *out_val = 0;
        return true;
    }
    if (op == 0x01) { /* OneOp */
        *out_val = 1;
        return true;
    }
    if (op == 0xFF) { /* OnesOp */
        *out_val = 0xFFFFFFFF;
        return true;
    }
    if (op == 0x0A && *pptr < end) { /* BytePrefix */
        *out_val = **pptr;
        (*pptr)++;
        return true;
    }
    if (op == 0x0B && *pptr + 1 < end) { /* WordPrefix */
        uint16_t val = 0;
        memcpy(&val, *pptr, 2);
        *pptr += 2;
        *out_val = val;
        return true;
    }
    if (op == 0x0C && *pptr + 3 < end) { /* DWordPrefix */
        uint32_t val = 0;
        memcpy(&val, *pptr, 4);
        *pptr += 4;
        *out_val = val;
        return true;
    }
    if (op == 0x0E && *pptr + 7 < end) { /* QWordPrefix */
        uint64_t val = 0;
        memcpy(&val, *pptr, 8);
        *pptr += 8;
        *out_val = (uint32_t)val;
        return true;
    }

    return false;
}

/* Bounded AML Package parser */
bool acpi_battery_parse_package(const uint8_t *ptr, const uint8_t *end, uint32_t *elements, size_t max_elements, size_t *num_found) {
    if (!ptr || !end || ptr >= end || !elements || max_elements == 0) return false;

    /* Locate PackageOp 0x12 */
    while (ptr < end && *ptr != 0x12) {
        ptr++;
    }
    if (ptr >= end || *ptr != 0x12) return false;
    ptr++; /* Skip 0x12 */

    if (ptr >= end) return false;

    /* Decode AML PkgLength */
    uint8_t lead = *ptr++;
    size_t pkg_length = 0;
    if ((lead & 0xC0) == 0) {
        pkg_length = lead & 0x3F;
    } else {
        int extra = (lead >> 6) & 0x03;
        pkg_length = lead & 0x0F;
        for (int b = 0; b < extra && ptr < end; b++) {
            pkg_length |= ((size_t)(*ptr++) << (4 + b * 8));
        }
    }
    (void)pkg_length;

    if (ptr >= end) return false;
    uint8_t count = *ptr++;
    size_t found = 0;

    for (uint8_t i = 0; i < count && ptr < end; i++) {
        uint32_t val = 0;
        if (parse_aml_integer(&ptr, end, &val)) {
            if (found < max_elements) {
                elements[found++] = val;
            }
        } else if (*ptr == 0x0D) { /* StringPrefix: skip string */
            ptr++;
            while (ptr < end && *ptr != 0) ptr++;
            if (ptr < end) ptr++; /* skip null terminator */
        } else {
            /* Skip unknown or sub-object */
            ptr++;
        }
    }

    if (num_found) *num_found = found;
    return found > 0;
}

/* Evaluate _BST (Battery Status): returns [State, PresentRate, RemainingCapacity, PresentVoltage] */
bool acpi_battery_evaluate_bst(const uint8_t *aml, size_t len, uint32_t *state, uint32_t *rate, uint32_t *rem_cap, uint32_t *voltage) {
    if (!aml || len < 8) return false;

    for (size_t i = 0; i + 4 <= len; i++) {
        if (memcmp(aml + i, "_BST", 4) == 0) {
            const uint8_t *start = aml + i + 4;
            const uint8_t *end = (start + 512 <= aml + len) ? (start + 512) : (aml + len);
            uint32_t elements[4] = {0};
            size_t found = 0;
            if (acpi_battery_parse_package(start, end, elements, 4, &found) && found >= 4) {
                if (state)   *state = elements[0];
                if (rate)    *rate = elements[1];
                if (rem_cap) *rem_cap = elements[2];
                if (voltage) *voltage = elements[3];
                return true;
            }
        }
    }
    return false;
}

/* Evaluate _BIX (Battery Information Extended): returns LastFullChargeCapacity at index 3 */
bool acpi_battery_evaluate_bix(const uint8_t *aml, size_t len, uint32_t *last_full_cap) {
    if (!aml || len < 8) return false;

    for (size_t i = 0; i + 4 <= len; i++) {
        if (memcmp(aml + i, "_BIX", 4) == 0) {
            const uint8_t *start = aml + i + 4;
            const uint8_t *end = (start + 512 <= aml + len) ? (start + 512) : (aml + len);
            uint32_t elements[8] = {0};
            size_t found = 0;
            if (acpi_battery_parse_package(start, end, elements, 8, &found) && found >= 4) {
                if (last_full_cap) *last_full_cap = elements[3];
                return true;
            }
        }
    }
    return false;
}

/* Evaluate _BIF (Battery Information fallback): returns LastFullChargeCapacity at index 2 */
bool acpi_battery_evaluate_bif(const uint8_t *aml, size_t len, uint32_t *last_full_cap) {
    if (!aml || len < 8) return false;

    for (size_t i = 0; i + 4 <= len; i++) {
        if (memcmp(aml + i, "_BIF", 4) == 0) {
            const uint8_t *start = aml + i + 4;
            const uint8_t *end = (start + 512 <= aml + len) ? (start + 512) : (aml + len);
            uint32_t elements[8] = {0};
            size_t found = 0;
            if (acpi_battery_parse_package(start, end, elements, 8, &found) && found >= 3) {
                if (last_full_cap) *last_full_cap = elements[2];
                return true;
            }
        }
    }
    return false;
}

/* Evaluate _PSR (Power Source): returns 1 for AC online, 0 for offline */
bool acpi_ac_adapter_evaluate_psr(const uint8_t *aml, size_t len, uint32_t *online) {
    if (!aml || !online || len < 5) return false;
    for (size_t i = 0; i + 4 <= len; i++) {
        if (memcmp(aml + i, "_PSR", 4) == 0) {
            const uint8_t *ptr = aml + i + 4;
            const uint8_t *end = aml + len;
            if (i >= 2 && aml[i - 2] == 0x14) {
                if (ptr < end) ptr++; /* method flags */
            }
            size_t ahead = 0;
            while (ptr < end && ahead < 32) {
                if (*ptr == 0xA4) {
                    ptr++;
                    uint32_t val = 0;
                    if (parse_aml_integer(&ptr, end, &val)) {
                        *online = (val != 0) ? AC_POWER_ONLINE : AC_POWER_OFFLINE;
                        return true;
                    }
                }
                uint32_t direct_val = 0;
                const uint8_t *save = ptr;
                if (parse_aml_integer(&ptr, end, &direct_val)) {
                    *online = (direct_val != 0) ? AC_POWER_ONLINE : AC_POWER_OFFLINE;
                    return true;
                }
                ptr = save + 1;
                ahead++;
            }
        }
    }
    return false;
}

/* Evaluate _TMP (Temperature): returns temperature in Celsius from tenths of Kelvin */
bool acpi_thermal_evaluate_tmp(const uint8_t *aml, size_t len, int32_t *temp_c) {
    if (!aml || !temp_c || len < 5) return false;
    for (size_t i = 0; i + 4 <= len; i++) {
        if (memcmp(aml + i, "_TMP", 4) == 0) {
            const uint8_t *ptr = aml + i + 4;
            const uint8_t *end = aml + len;
            if (i >= 2 && aml[i - 2] == 0x14) {
                if (ptr < end) ptr++;
            }
            size_t ahead = 0;
            while (ptr < end && ahead < 32) {
                if (*ptr == 0xA4) {
                    ptr++;
                    uint32_t val = 0;
                    if (parse_aml_integer(&ptr, end, &val)) {
                        if (val >= 2732 && val <= 3932) {
                            *temp_c = (int32_t)((val - 2732) / 10);
                            return true;
                        }
                    }
                }
                uint32_t direct_val = 0;
                const uint8_t *save = ptr;
                if (parse_aml_integer(&ptr, end, &direct_val)) {
                    if (direct_val >= 2732 && direct_val <= 3932) {
                        *temp_c = (int32_t)((direct_val - 2732) / 10);
                        return true;
                    }
                }
                ptr = save + 1;
                ahead++;
            }
        }
    }
    return false;
}

/* Status computation and clamping */
void acpi_battery_compute_status(uint32_t bst_state, uint32_t present_rate, uint32_t rem_cap, uint32_t voltage, uint32_t full_cap, battery_status_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    /* Calculate Capacity Percentage */
    if (full_cap > 0) {
        uint64_t cap = ((uint64_t)rem_cap * 100) / full_cap;
        if (cap > 100) cap = 100;
        out->capacity_percent = (uint32_t)cap;
    } else {
        out->capacity_percent = 0;
    }

    /* Determine State: 0 = Unknown, 1 = Discharging, 2 = Charging, 3 = Full */
    if ((bst_state & 0x02) != 0) {
        out->state = BATTERY_STATE_CHARGING; /* 2 */
    } else if ((bst_state & 0x01) != 0) {
        out->state = BATTERY_STATE_DISCHARGING; /* 1 */
    } else if (out->capacity_percent >= 99 || (bst_state == 0 && rem_cap > 0 && rem_cap >= full_cap)) {
        out->state = BATTERY_STATE_FULL; /* 3 */
    } else if (bst_state == 0 && rem_cap > 0) {
        out->state = (out->capacity_percent >= 95) ? BATTERY_STATE_FULL : BATTERY_STATE_UNKNOWN;
    } else {
        out->state = BATTERY_STATE_UNKNOWN; /* 0 */
    }

    /* Calculate Time Remaining:
     * If PresentRate is non-zero and state is Discharging, (RemainingCapacity * 60) / PresentRate.
     * Clamp to 9999 minutes. */
    if (out->state == BATTERY_STATE_DISCHARGING && present_rate > 0) {
        uint64_t mins = ((uint64_t)rem_cap * 60) / present_rate;
        if (mins > 9999) mins = 9999;
        out->time_remaining_mins = (uint32_t)mins;
    } else {
        out->time_remaining_mins = 0;
    }

    out->voltage_mv = voltage;

    /* AC Online & Thermal: derive default from state, then apply mock or ACPI */
    if (out->state == BATTERY_STATE_CHARGING || out->state == BATTERY_STATE_FULL) {
        out->ac_online = AC_POWER_ONLINE;
    } else if (out->state == BATTERY_STATE_DISCHARGING) {
        out->ac_online = AC_POWER_OFFLINE;
    } else {
        out->ac_online = AC_POWER_UNKNOWN;
    }
    if (g_mock_ac_online != AC_POWER_UNKNOWN) {
        out->ac_online = g_mock_ac_online;
    }

    out->temperature_c = g_mock_temp_c;
}

#ifndef HOST_TEST
/* Read battery status from DSDT or ACPI EC on hardware */
static bool poll_hardware_battery(battery_status_t *out) {
    acpi_sdt_header_t *facp = acpi_find_table("FACP");
    if (!facp) return false;
    const acpi_fadt_t *fadt = (const acpi_fadt_t *)facp;
    uintptr_t hhdm = acpi_get_hhdm_offset();

    uintptr_t dsdt_phys = 0;
    if (fadt->header.length >= 148 && fadt->x_dsdt != 0) {
        dsdt_phys = (uintptr_t)fadt->x_dsdt;
    } else if (fadt->header.length >= 44 && fadt->dsdt != 0) {
        dsdt_phys = (uintptr_t)fadt->dsdt;
    }
    if (dsdt_phys == 0) return false;

    if (!acpi_ensure_mapped(dsdt_phys, sizeof(acpi_sdt_header_t))) return false;
    const acpi_sdt_header_t *dsdt = (const acpi_sdt_header_t *)(dsdt_phys + hhdm);
    if (dsdt->length < sizeof(acpi_sdt_header_t) || dsdt->length > MAX_ACPI_TABLE_SIZE) return false;
    if (!acpi_ensure_mapped(dsdt_phys, dsdt->length)) return false;

    const uint8_t *aml = (const uint8_t *)dsdt;
    size_t len = dsdt->length;

    size_t dev_offset = 0;
    if (!acpi_battery_locate(aml, len, &dev_offset)) {
        return false;
    }

    /* Try to evaluate _BST and _BIX/_BIF from DSDT */
    uint32_t bst_state = 0, rate = 0, rem_cap = 0, voltage = 0;
    uint32_t full_cap = 0;

    bool has_bst = acpi_battery_evaluate_bst(aml, len, &bst_state, &rate, &rem_cap, &voltage);
    bool has_bix = acpi_battery_evaluate_bix(aml, len, &full_cap);
    if (!has_bix) {
        acpi_battery_evaluate_bif(aml, len, &full_cap);
    }

    /* If AML yielded valid values, compute status */
    if (has_bst && (full_cap > 0 || rem_cap > 0)) {
        acpi_battery_compute_status(bst_state, rate, rem_cap, voltage, full_cap, out);
        uint32_t ac_on = AC_POWER_UNKNOWN;
        if (acpi_ac_adapter_evaluate_psr(aml, len, &ac_on)) {
            out->ac_online = ac_on;
        }
        int32_t tc = 0;
        if (acpi_thermal_evaluate_tmp(aml, len, &tc)) {
            out->temperature_c = tc;
        }
        return true;
    }

    /* Fallback: Direct EC query for standard Smart Battery registers (ports 0x62/0x66) */
    uint8_t ec_state = 0;
    uint8_t ec_cap_lo = 0, ec_cap_hi = 0;
    uint8_t ec_rate_lo = 0, ec_rate_hi = 0;
    uint8_t ec_full_lo = 0, ec_full_hi = 0;
    uint8_t ec_volt_lo = 0, ec_volt_hi = 0;

    /* Common standard ACPI EC battery registers */
    if (acpi_ec_read(0x10, &ec_state) == 0 &&
        acpi_ec_read(0x11, &ec_cap_lo) == 0 && acpi_ec_read(0x12, &ec_cap_hi) == 0) {
        acpi_ec_read(0x13, &ec_rate_lo);
        acpi_ec_read(0x14, &ec_rate_hi);
        acpi_ec_read(0x15, &ec_full_lo);
        acpi_ec_read(0x16, &ec_full_hi);
        acpi_ec_read(0x17, &ec_volt_lo);
        acpi_ec_read(0x18, &ec_volt_hi);

        uint32_t c_state = ec_state;
        uint32_t c_rem = (uint32_t)ec_cap_lo | ((uint32_t)ec_cap_hi << 8);
        uint32_t c_rate = (uint32_t)ec_rate_lo | ((uint32_t)ec_rate_hi << 8);
        uint32_t c_full = (uint32_t)ec_full_lo | ((uint32_t)ec_full_hi << 8);
        uint32_t c_volt = (uint32_t)ec_volt_lo | ((uint32_t)ec_volt_hi << 8);

        acpi_battery_compute_status(c_state, c_rate, c_rem, c_volt, c_full, out);
        uint32_t ac_on = AC_POWER_UNKNOWN;
        if (acpi_ac_adapter_evaluate_psr(aml, len, &ac_on)) {
            out->ac_online = ac_on;
        }
        int32_t tc = 0;
        if (acpi_thermal_evaluate_tmp(aml, len, &tc)) {
            out->temperature_c = tc;
        }
        return true;
    }

    return false;
}
#endif

void acpi_battery_init(void) {
    g_battery_initialized = true;
    g_battery_present = false;
    g_has_cached = false;
    memset(&g_cached_battery, 0, sizeof(g_cached_battery));

#ifndef HOST_TEST
    battery_status_t status;
    if (poll_hardware_battery(&status)) {
        g_battery_present = true;
        g_cached_battery = status;
        g_last_update_ticks = get_current_ticks();
        g_has_cached = true;
        serial_puts("[BATTERY] Control Method Battery (PNP0C0A) initialized\n");
    } else {
        serial_puts("[BATTERY] No ACPI battery found; desktop battery icon will be hidden\n");
    }
#else
    if (g_ec_reader) {
        g_battery_present = true;
    }
#endif
}

int acpi_battery_get(battery_status_t *out) {
    if (!out) return -14; /* SYSCALL_EFAULT */

#ifdef HOST_TEST
    if (g_ec_reader) {
        g_battery_present = true;
    }
#endif

    if (!g_battery_initialized) {
        acpi_battery_init();
    }

    if (!g_battery_present) {
        return SYSCALL_ENODEV; /* -35 */
    }

    uint64_t now = get_current_ticks();
    uint64_t freq = get_tick_frequency();
    uint64_t cache_period = 5 * freq; /* 5 seconds */

    /* If cached and within 5 seconds, return immediately without touching slow EC */
    if (g_has_cached && (now - g_last_update_ticks) < cache_period) {
        memcpy(out, &g_cached_battery, sizeof(battery_status_t));
        return 0;
    }

    /* Cache expired or state changed: refresh from hardware/EC */
    battery_status_t refreshed;
    bool ok = false;
#ifndef HOST_TEST
    ok = poll_hardware_battery(&refreshed);
#else
    if (g_ec_reader) {
        uint8_t ec_state = 0, cap_lo = 0, cap_hi = 0, rate_lo = 0, rate_hi = 0, full_lo = 0, full_hi = 0, volt_lo = 0, volt_hi = 0;
        if (g_ec_reader(0x10, &ec_state) == 0 &&
            g_ec_reader(0x11, &cap_lo) == 0 && g_ec_reader(0x12, &cap_hi) == 0) {
            g_ec_reader(0x13, &rate_lo); g_ec_reader(0x14, &rate_hi);
            g_ec_reader(0x15, &full_lo); g_ec_reader(0x16, &full_hi);
            g_ec_reader(0x17, &volt_lo); g_ec_reader(0x18, &volt_hi);
            uint32_t c_state = ec_state;
            uint32_t c_rem = (uint32_t)cap_lo | ((uint32_t)cap_hi << 8);
            uint32_t c_rate = (uint32_t)rate_lo | ((uint32_t)rate_hi << 8);
            uint32_t c_full = (uint32_t)full_lo | ((uint32_t)full_hi << 8);
            uint32_t c_volt = (uint32_t)volt_lo | ((uint32_t)volt_hi << 8);
            acpi_battery_compute_status(c_state, c_rate, c_rem, c_volt, c_full, &refreshed);
            ok = true;
        }
    }
#endif

    if (ok) {
        /* State transition check (e.g. Charging <-> Discharging) updates immediately */
        g_cached_battery = refreshed;
        g_last_update_ticks = now;
        g_has_cached = true;
    }

    memcpy(out, &g_cached_battery, sizeof(battery_status_t));
    return 0;
}
