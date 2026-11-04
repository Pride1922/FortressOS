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
static uint16_t g_ec_data_port = ACPI_EC_DATA_PORT;
static uint16_t g_ec_cmd_port = ACPI_EC_CMD_PORT;
static int32_t g_mock_temp_c = 0;
static uint32_t g_mock_ac_online = AC_POWER_UNKNOWN;
static acpi_ec_battery_map_t g_ec_map;
static bool g_ec_map_discovered = false;

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

void acpi_battery_set_ec_ports(uint16_t data_port, uint16_t cmd_port) {
    g_ec_data_port = data_port ? data_port : ACPI_EC_DATA_PORT;
    g_ec_cmd_port = cmd_port ? cmd_port : ACPI_EC_CMD_PORT;
}

void acpi_battery_get_ec_ports(uint16_t *data_port, uint16_t *cmd_port) {
    if (data_port) *data_port = g_ec_data_port;
    if (cmd_port) *cmd_port = g_ec_cmd_port;
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
    g_ec_data_port = ACPI_EC_DATA_PORT;
    g_ec_cmd_port = ACPI_EC_CMD_PORT;
    g_mock_temp_c = 0;
    g_mock_ac_online = AC_POWER_UNKNOWN;
    memset(&g_ec_map, 0, sizeof(g_ec_map));
    g_ec_map_discovered = false;
}

void acpi_battery_force_update(void) {
    g_has_cached = false;
}

/* Parse ECDT (Embedded Controller Boot Resources Table) for dynamic I/O ports */
bool acpi_parse_ecdt(const uint8_t *table, size_t len, uint16_t *cmd_port, uint16_t *data_port) {
    if (!table || len < 36 + 24) return false;
    if (memcmp(table, "ECDT", 4) != 0) return false;

    /* ec_control GAS is at offset 36: address_space byte at +0, address uint64 at +4 */
    uint8_t ctrl_space = table[36];
    uint64_t ctrl_addr = 0;
    memcpy(&ctrl_addr, table + 40, 8);

    /* ec_data GAS is at offset 48: address_space byte at +0, address uint64 at +4 */
    uint8_t data_space = table[48];
    uint64_t data_addr = 0;
    memcpy(&data_addr, table + 52, 8);

    /* AddressSpace 1 is System I/O */
    if (ctrl_space == 1 && data_space == 1 &&
        ctrl_addr > 0 && ctrl_addr <= 0xFFFF &&
        data_addr > 0 && data_addr <= 0xFFFF) {
        if (cmd_port) *cmd_port = (uint16_t)ctrl_addr;
        if (data_port) *data_port = (uint16_t)data_addr;
        return true;
    }
    return false;
}

/* Standard EC Bus I/O with bounded polling timeouts (~1-2ms) */
int acpi_ec_read(uint8_t addr, uint8_t *val) {
    if (!val) return -1;
    if (g_ec_reader) {
        return g_ec_reader(addr, val);
    }
#ifndef HOST_TEST
    uint16_t cmd_port = g_ec_cmd_port ? g_ec_cmd_port : ACPI_EC_CMD_PORT;
    uint16_t data_port = g_ec_data_port ? g_ec_data_port : ACPI_EC_DATA_PORT;

    uint8_t status = inb(cmd_port);
    if (status == 0xFF) return -1; /* Floating bus: no EC present */

    /* Pre-drain stale unread byte in data register if OBF is asserted */
    if ((status & ACPI_EC_FLAG_OBF) != 0) {
        (void)inb(data_port);
    }

    /* 1. Wait for IBF == 0 */
    for (uint32_t i = 0; i < 50000; i++) {
        status = inb(cmd_port);
        if (status == 0xFF) return -1;
        if ((status & ACPI_EC_FLAG_IBF) == 0) break;
        io_wait();
        if (i == 49999) {
            serial_puts("[BATTERY] acpi_ec_read step 1 (IBF=0) timeout, status=");
            serial_print_hex(status);
            serial_puts("\n");
            return -1;
        }
    }

    /* 2. Send RD_EC command */
    outb(cmd_port, ACPI_EC_CMD_RD);

    /* 3. Wait for IBF == 0 */
    for (uint32_t i = 0; i < 50000; i++) {
        status = inb(cmd_port);
        if (status == 0xFF) return -1;
        if ((status & ACPI_EC_FLAG_IBF) == 0) break;
        io_wait();
        if (i == 49999) {
            serial_puts("[BATTERY] acpi_ec_read step 3 (IBF=0) timeout, status=");
            serial_print_hex(status);
            serial_puts("\n");
            return -1;
        }
    }

    /* 4. Send EC register address */
    outb(data_port, addr);

    /* 5. Wait for OBF == 1 */
    for (uint32_t i = 0; i < 50000; i++) {
        status = inb(cmd_port);
        if (status == 0xFF) return -1;
        if ((status & ACPI_EC_FLAG_OBF) != 0) break;
        io_wait();
        if (i == 49999) {
            serial_puts("[BATTERY] acpi_ec_read step 5 (OBF=1) timeout, status=");
            serial_print_hex(status);
            serial_puts("\n");
            return -1;
        }
    }

    /* 6. Read returned data */
    *val = inb(data_port);
    return 0;
#else
    (void)addr;
    return -1;
#endif
}

/* Bounded AML PNP0C0A device search */
bool acpi_battery_locate(const uint8_t *aml, size_t len, size_t *out_offset) {
    if (!aml || len < 8) return false;

    /* Search for string "PNP0C0A" or EISA ID, device name, or method */
    for (size_t i = 0; i + 4 <= len; i++) {
        if (i + 7 <= len && (memcmp(aml + i, "PNP0C0A", 7) == 0 || memcmp(aml + i, "pnp0c0a", 7) == 0)) {
            if (out_offset) *out_offset = i;
            return true;
        }
        /* EISA ID PNP0C0A patterns */
        if ((aml[i] == 0x41 && aml[i+1] == 0xD0 && aml[i+2] == 0x0C && aml[i+3] == 0x0A) ||
            (aml[i] == 0xD0 && aml[i+1] == 0x41 && aml[i+2] == 0x0C && aml[i+3] == 0x0A) ||
            (aml[i] == 0x0A && aml[i+1] == 0x0C && aml[i+2] == 0xD0 && aml[i+3] == 0x41) ||
            (aml[i] == 0x0C && aml[i+1] == 0x41 && aml[i+2] == 0xD0 && aml[i+3] == 0x0C)) {
            if (out_offset) *out_offset = i;
            return true;
        }
        /* Method _BST/_BIX/_BIF or Device BAT0/BAT1/BAT2/BATC */
        if (memcmp(aml + i, "_BST", 4) == 0 ||
            memcmp(aml + i, "_BIX", 4) == 0 ||
            memcmp(aml + i, "_BIF", 4) == 0 ||
            memcmp(aml + i, "BAT0", 4) == 0 ||
            memcmp(aml + i, "BAT1", 4) == 0 ||
            memcmp(aml + i, "BAT2", 4) == 0 ||
            memcmp(aml + i, "BATC", 4) == 0) {
            if (out_offset) *out_offset = i;
            return true;
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
    } else if (bst_state == 0 && full_cap > 0 && rem_cap >= full_cap) {
        out->state = BATTERY_STATE_FULL;
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

    /* Battery state alone is not an adapter-presence measurement. */
    out->ac_online = AC_POWER_UNKNOWN;
    if (g_mock_ac_online != AC_POWER_UNKNOWN) {
        out->ac_online = g_mock_ac_online;
    }

    /* Fallback state inference if state register was 0/unknown */
    if (out->state == BATTERY_STATE_UNKNOWN && out->ac_online != AC_POWER_UNKNOWN) {
        if (out->ac_online == AC_POWER_OFFLINE) {
            out->state = BATTERY_STATE_DISCHARGING;
        } else if (out->ac_online == AC_POWER_ONLINE) {
            out->state = (out->capacity_percent >= 98) ? BATTERY_STATE_FULL : BATTERY_STATE_CHARGING;
        }
    }

    out->temperature_c = g_mock_temp_c;
}

/* Helper to decode AML PkgLength */
static inline size_t decode_pkg_length(const uint8_t *ptr, const uint8_t *end, size_t *bytes_consumed) {
    if (!ptr || ptr >= end) {
        if (bytes_consumed) *bytes_consumed = 0;
        return 0;
    }
    uint8_t lead = *ptr++;
    size_t len = 0;
    if ((lead & 0xC0) == 0) {
        len = lead & 0x3F;
        if (bytes_consumed) *bytes_consumed = 1;
    } else {
        int extra = (lead >> 6) & 0x03;
        if (ptr + extra > end) {
            if (bytes_consumed) *bytes_consumed = 0;
            return 0;
        }
        len = lead & 0x0F;
        for (int b = 0; b < extra; b++) {
            len |= ((size_t)(*ptr++) << (4 + b * 8));
        }
        if (bytes_consumed) *bytes_consumed = 1 + extra;
    }
    return len;
}

/* Helper to skip AML NameString (SimpleName, DualNamePath, MultiNamePath, RootPath, PrefixChar) */
static inline const uint8_t *skip_aml_namestring(const uint8_t *ptr, const uint8_t *end) {
    if (!ptr || ptr >= end) return ptr;
    while (ptr < end && (*ptr == '\\' || *ptr == '^')) {
        ptr++;
    }
    if (ptr >= end) return ptr;

    if (*ptr == 0x00) { /* NullName */
        return ptr + 1;
    }
    if (*ptr == 0x2E) { /* DualNamePrefix: 2 NameSegs (8 bytes) */
        ptr++;
        return (ptr + 8 <= end) ? (ptr + 8) : end;
    }
    if (*ptr == 0x2F) { /* MultiNamePrefix: SegCount byte + SegCount * 4 bytes */
        ptr++;
        if (ptr >= end) return end;
        uint8_t count = *ptr++;
        size_t total = (size_t)count * 4;
        return (ptr + total <= end) ? (ptr + total) : end;
    }
    /* Simple 4-byte NameSeg */
    return (ptr + 4 <= end) ? (ptr + 4) : end;
}

/* Parse AML Field definitions inside EmbeddedControl OperationRegions */
bool acpi_parse_ec_fields(const uint8_t *aml, size_t len, acpi_ec_battery_map_t *map) {
    if (!aml || len < 8 || !map) return false;

    for (size_t i = 0; i + 6 <= len; i++) {
        /* Look for FieldOp: 0x5B, 0x81 */
        if (aml[i] == 0x5B && aml[i + 1] == 0x81) {
            const uint8_t *ptr = aml + i + 2;
            const uint8_t *end = aml + len;
            size_t consumed = 0;
            size_t field_len = decode_pkg_length(ptr, end, &consumed);
            if (consumed == 0 || field_len == 0) continue;
            const uint8_t *field_end = ptr + field_len;
            if (field_end > end) field_end = end;
            ptr += consumed;

            /* Skip region NameString */
            ptr = skip_aml_namestring(ptr, field_end);
            if (ptr >= field_end) continue;

            /* Skip FieldFlags (1 byte) */
            ptr++;

            size_t current_bit_offset = 0;
            while (ptr < field_end) {
                uint8_t op = *ptr;
                if (op == 0x00) { /* ReservedField (Offset) */
                    ptr++;
                    size_t c = 0;
                    size_t skip_bits = decode_pkg_length(ptr, field_end, &c);
                    if (c == 0) break;
                    ptr += c;
                    current_bit_offset += skip_bits;
                } else if (op == 0x01) { /* AccessField */
                    ptr += 3;
                } else if (op == 0x02) { /* ConnectField */
                    ptr++;
                    while (ptr < field_end && *ptr != 0 && *ptr < 0x20) ptr++;
                    if (ptr + 4 <= field_end) ptr += 4;
                } else if (op == 0x03) { /* ExtendedAccessField */
                    ptr += 4;
                } else if ((op >= 'A' && op <= 'Z') || (op >= '0' && op <= '9') || op == '_') {
                    /* NamedField */
                    if (ptr + 4 > field_end) break;
                    char name[5];
                    memcpy(name, ptr, 4);
                    name[4] = '\0';
                    ptr += 4;

                    size_t c = 0;
                    size_t bit_len = decode_pkg_length(ptr, field_end, &c);
                    if (c == 0) break;
                    ptr += c;

                    size_t byte_off = current_bit_offset / 8;

                    /* Match field names against known battery field patterns (standard, Dell, Smart Battery) */
                    if (!map->found_state && (
                        memcmp(name, "B1ST", 4) == 0 || memcmp(name, "B0ST", 4) == 0 ||
                        memcmp(name, "BST0", 4) == 0 || memcmp(name, "BST1", 4) == 0 ||
                        memcmp(name, "BA1S", 4) == 0 || memcmp(name, "BATS", 4) == 0 ||
                        memcmp(name, "ECST", 4) == 0 || memcmp(name, "ST01", 4) == 0 ||
                        memcmp(name, "ST00", 4) == 0 || memcmp(name, "SBBS", 4) == 0 ||
                        memcmp(name, "SBST", 4) == 0 || memcmp(name, "MBST", 4) == 0 ||
                        memcmp(name, "PBST", 4) == 0 || memcmp(name, "BST_", 4) == 0 ||
                        memcmp(name, "BSTP", 4) == 0)) {
                        map->state_offset = (uint8_t)byte_off;
                        map->state_bits = (uint8_t)bit_len;
                        map->found_state = true;
                    } else if (!map->found_rem && (
                        memcmp(name, "B1RC", 4) == 0 || memcmp(name, "B0RC", 4) == 0 ||
                        memcmp(name, "BRC0", 4) == 0 || memcmp(name, "BRC1", 4) == 0 ||
                        memcmp(name, "RCAP", 4) == 0 || memcmp(name, "BARC", 4) == 0 ||
                        memcmp(name, "CAP0", 4) == 0 || memcmp(name, "CAP1", 4) == 0 ||
                        memcmp(name, "RCP0", 4) == 0 || memcmp(name, "RCP1", 4) == 0 ||
                        memcmp(name, "SBRC", 4) == 0 || memcmp(name, "RC01", 4) == 0 ||
                        memcmp(name, "MBRM", 4) == 0 || memcmp(name, "BPRC", 4) == 0 ||
                        memcmp(name, "PBRC", 4) == 0 || memcmp(name, "BRMC", 4) == 0 ||
                        memcmp(name, "RC00", 4) == 0 || memcmp(name, "REMC", 4) == 0)) {
                        map->rem_cap_offset = (uint8_t)byte_off;
                        map->rem_cap_bits = (uint8_t)bit_len;
                        map->found_rem = true;
                    } else if (!map->found_full && (
                        memcmp(name, "B1FC", 4) == 0 || memcmp(name, "B0FC", 4) == 0 ||
                        memcmp(name, "BFC0", 4) == 0 || memcmp(name, "BFC1", 4) == 0 ||
                        memcmp(name, "FCAP", 4) == 0 || memcmp(name, "BAFC", 4) == 0 ||
                        memcmp(name, "B1DC", 4) == 0 || memcmp(name, "B0DC", 4) == 0 ||
                        memcmp(name, "BDC1", 4) == 0 || memcmp(name, "BDC0", 4) == 0 ||
                        memcmp(name, "FC01", 4) == 0 || memcmp(name, "FCP0", 4) == 0 ||
                        memcmp(name, "FCP1", 4) == 0 || memcmp(name, "DCP0", 4) == 0 ||
                        memcmp(name, "DCP1", 4) == 0 || memcmp(name, "SBFC", 4) == 0 ||
                        memcmp(name, "SBDC", 4) == 0 || memcmp(name, "MBFC", 4) == 0 ||
                        memcmp(name, "BMAX", 4) == 0 || memcmp(name, "PBFC", 4) == 0 ||
                        memcmp(name, "BFCC", 4) == 0 || memcmp(name, "FC00", 4) == 0 ||
                        memcmp(name, "FULC", 4) == 0)) {
                        map->full_cap_offset = (uint8_t)byte_off;
                        map->full_cap_bits = (uint8_t)bit_len;
                        map->found_full = true;
                    } else if (!map->found_rate && (
                        memcmp(name, "B1PR", 4) == 0 || memcmp(name, "B0PR", 4) == 0 ||
                        memcmp(name, "BPR0", 4) == 0 || memcmp(name, "BPR1", 4) == 0 ||
                        memcmp(name, "RATE", 4) == 0 || memcmp(name, "BAPR", 4) == 0 ||
                        memcmp(name, "CR01", 4) == 0 || memcmp(name, "CRT0", 4) == 0 ||
                        memcmp(name, "CRT1", 4) == 0 || memcmp(name, "BAC0", 4) == 0 ||
                        memcmp(name, "BAC1", 4) == 0 || memcmp(name, "SBAC", 4) == 0 ||
                        memcmp(name, "MBPR", 4) == 0 || memcmp(name, "MBCR", 4) == 0 ||
                        memcmp(name, "PBPR", 4) == 0 || memcmp(name, "PBCR", 4) == 0 ||
                        memcmp(name, "CR00", 4) == 0 || memcmp(name, "PR00", 4) == 0)) {
                        map->rate_offset = (uint8_t)byte_off;
                        map->rate_bits = (uint8_t)bit_len;
                        map->found_rate = true;
                    } else if (!map->found_volt && (
                        memcmp(name, "B1PV", 4) == 0 || memcmp(name, "B0PV", 4) == 0 ||
                        memcmp(name, "BPV0", 4) == 0 || memcmp(name, "BPV1", 4) == 0 ||
                        memcmp(name, "VOLT", 4) == 0 || memcmp(name, "BAPV", 4) == 0 ||
                        memcmp(name, "PV01", 4) == 0 || memcmp(name, "VOT0", 4) == 0 ||
                        memcmp(name, "VOT1", 4) == 0 || memcmp(name, "VL00", 4) == 0 ||
                        memcmp(name, "VL01", 4) == 0 || memcmp(name, "SBVO", 4) == 0 ||
                        memcmp(name, "MBVO", 4) == 0 || memcmp(name, "PBVO", 4) == 0 ||
                        memcmp(name, "PV00", 4) == 0 || memcmp(name, "VOL0", 4) == 0)) {
                        map->volt_offset = (uint8_t)byte_off;
                        map->volt_bits = (uint8_t)bit_len;
                        map->found_volt = true;
                    }

                    current_bit_offset += bit_len;
                } else {
                    ptr++;
                }
            }
        }
    }
    return (map->found_rem && map->found_full);
}

/* Read live battery status from Embedded Controller using discovered mapping */
bool acpi_read_ec_battery(const acpi_ec_battery_map_t *map, battery_status_t *out) {
    if (!map || !out) return false;
    if (!map->found_rem || !map->found_full) return false;

    uint32_t state = 0, rate = 0, rem_cap = 0, full_cap = 0, volt = 0;

    /* Read Remaining Capacity */
    if (map->rem_cap_bits == 8) {
        uint8_t b = 0;
        if (acpi_ec_read(map->rem_cap_offset, &b) != 0) return false;
        rem_cap = b;
    } else {
        uint8_t lo = 0, hi = 0;
        if (acpi_ec_read(map->rem_cap_offset, &lo) != 0) return false;
        if (acpi_ec_read(map->rem_cap_offset + 1, &hi) != 0) return false;
        rem_cap = (uint32_t)lo | ((uint32_t)hi << 8);
    }

    /* Read Full Capacity */
    if (map->full_cap_bits == 8) {
        uint8_t b = 0;
        if (acpi_ec_read(map->full_cap_offset, &b) != 0) return false;
        full_cap = b;
    } else {
        uint8_t lo = 0, hi = 0;
        if (acpi_ec_read(map->full_cap_offset, &lo) != 0) return false;
        if (acpi_ec_read(map->full_cap_offset + 1, &hi) != 0) return false;
        full_cap = (uint32_t)lo | ((uint32_t)hi << 8);
    }

    if (full_cap == 0) return false;

    /* Read State */
    if (map->found_state) {
        uint8_t lo = 0, hi = 0;
        if (acpi_ec_read(map->state_offset, &lo) == 0) {
            state = lo;
            if (map->state_bits > 8 && acpi_ec_read(map->state_offset + 1, &hi) == 0) {
                state |= ((uint32_t)hi << 8);
            }
        }
    }

    /* Read Rate */
    if (map->found_rate) {
        uint8_t lo = 0, hi = 0;
        if (acpi_ec_read(map->rate_offset, &lo) == 0 &&
            acpi_ec_read(map->rate_offset + 1, &hi) == 0) {
            rate = (uint32_t)lo | ((uint32_t)hi << 8);
        }
    }

    /* Read Voltage */
    if (map->found_volt) {
        uint8_t lo = 0, hi = 0;
        if (acpi_ec_read(map->volt_offset, &lo) == 0 &&
            acpi_ec_read(map->volt_offset + 1, &hi) == 0) {
            volt = (uint32_t)lo | ((uint32_t)hi << 8);
        }
    }

    acpi_battery_compute_status(state, rate, rem_cap, volt, full_cap, out);
    return true;
}

static bool poll_hardware_battery(battery_status_t *out) {
    if (!out) return false;

#ifndef HOST_TEST
    const acpi_sdt_header_t *tables[32];
    size_t num_tables = 0;

    const acpi_sdt_header_t *dsdt = acpi_get_dsdt();
    if (dsdt && num_tables < 32) {
        tables[num_tables++] = dsdt;
    }

    size_t sdt_count = acpi_get_table_count();
    for (size_t i = 0; i < sdt_count && num_tables < 32; i++) {
        acpi_sdt_header_t *hdr = acpi_get_table_by_index(i);
        if (hdr && (memcmp(hdr->signature, "SSDT", 4) == 0 ||
                    memcmp(hdr->signature, "DSDT", 4) == 0 ||
                    memcmp(hdr->signature, "ECDT", 4) == 0)) {
            tables[num_tables++] = hdr;
        }
    }

    if (num_tables == 0) {
        return false;
    }

    /* Check for ECDT to retrieve dynamic EC ports */
    for (size_t t = 0; t < num_tables; t++) {
        uint16_t c_port = 0, d_port = 0;
        if (acpi_parse_ecdt((const uint8_t *)tables[t], tables[t]->length, &c_port, &d_port)) {
            g_ec_cmd_port = c_port;
            g_ec_data_port = d_port;
            serial_puts("[BATTERY] ECDT active: cmd=0x");
            serial_print_hex(c_port);
            serial_puts(" data=0x");
            serial_print_hex(d_port);
            serial_puts("\n");
            break;
        }
    }

    /* Check if battery device is present in ACPI tables */
    bool bat_found = false;
    for (size_t t = 0; t < num_tables; t++) {
        size_t off = 0;
        if (acpi_battery_locate((const uint8_t *)tables[t], tables[t]->length, &off)) {
            bat_found = true;
            break;
        }
    }
    if (!bat_found) {
        g_battery_present = false;
        return false;
    }
    g_battery_present = true;

    /* Discover EC field mapping if not yet discovered */
    if (!g_ec_map_discovered) {
        memset(&g_ec_map, 0, sizeof(g_ec_map));
        for (size_t t = 0; t < num_tables; t++) {
            if (acpi_parse_ec_fields((const uint8_t *)tables[t], tables[t]->length, &g_ec_map)) {
                g_ec_map_discovered = true;
                serial_puts("[BATTERY] EC map discovered: rem_off=");
                serial_print_dec(g_ec_map.rem_cap_offset);
                serial_puts(" full_off=");
                serial_print_dec(g_ec_map.full_cap_offset);
                serial_puts("\n");
                break;
            }
        }
    }

    /* Read AC adapter and thermal zone from tables */
    uint32_t ac_on = AC_POWER_UNKNOWN;
    int32_t tc = 0;
    bool has_temp = false;
    for (size_t t = 0; t < num_tables; t++) {
        if (ac_on == AC_POWER_UNKNOWN) {
            acpi_ac_adapter_evaluate_psr((const uint8_t *)tables[t], tables[t]->length, &ac_on);
        }
        if (!has_temp) {
            has_temp = acpi_thermal_evaluate_tmp((const uint8_t *)tables[t], tables[t]->length, &tc);
        }
    }

    /* 1. Try reading live registers from Embedded Controller via discovered map */
    if (g_ec_map.found_rem && g_ec_map.found_full) {
        if (acpi_read_ec_battery(&g_ec_map, out)) {
            if (ac_on != AC_POWER_UNKNOWN) out->ac_online = ac_on;
            if (has_temp) out->temperature_c = tc;
            if (out->state == BATTERY_STATE_UNKNOWN && ac_on != AC_POWER_UNKNOWN) {
                if (ac_on == AC_POWER_OFFLINE) {
                    out->state = BATTERY_STATE_DISCHARGING;
                } else if (ac_on == AC_POWER_ONLINE) {
                    out->state = (out->capacity_percent >= 98) ? BATTERY_STATE_FULL : BATTERY_STATE_CHARGING;
                }
            }
            return true;
        }
    }

    /* 2. Try static AML packages (_BST and _BIX/_BIF) */
    uint32_t bst_state = 0, rate = 0, rem_cap = 0, volt = 0, full_cap = 0;
    bool has_bst = false, has_full = false;
    for (size_t t = 0; t < num_tables; t++) {
        const uint8_t *aml = (const uint8_t *)tables[t];
        size_t len = tables[t]->length;
        if (!has_bst) {
            has_bst = acpi_battery_evaluate_bst(aml, len, &bst_state, &rate, &rem_cap, &volt);
        }
        if (!has_full) {
            has_full = acpi_battery_evaluate_bix(aml, len, &full_cap);
            if (!has_full) {
                has_full = acpi_battery_evaluate_bif(aml, len, &full_cap);
            }
        }
    }
    if (has_bst && full_cap > 0) {
        acpi_battery_compute_status(bst_state, rate, rem_cap, volt, full_cap, out);
        if (ac_on != AC_POWER_UNKNOWN) out->ac_online = ac_on;
        if (has_temp) out->temperature_c = tc;
        return true;
    }

    return false;
#else
    /* HOST_TEST path */
    if (g_ec_map.found_rem && g_ec_map.found_full) {
        if (acpi_read_ec_battery(&g_ec_map, out)) {
            if (g_mock_ac_online != AC_POWER_UNKNOWN) out->ac_online = g_mock_ac_online;
            out->temperature_c = g_mock_temp_c;
            if (out->state == BATTERY_STATE_UNKNOWN && out->ac_online != AC_POWER_UNKNOWN) {
                if (out->ac_online == AC_POWER_OFFLINE) {
                    out->state = BATTERY_STATE_DISCHARGING;
                } else if (out->ac_online == AC_POWER_ONLINE) {
                    out->state = (out->capacity_percent >= 98) ? BATTERY_STATE_FULL : BATTERY_STATE_CHARGING;
                }
            }
            return true;
        }
    }
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
            acpi_battery_compute_status(c_state, c_rate, c_rem, c_volt, c_full, out);
            return true;
        }
    }
    return false;
#endif
}

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
        serial_puts("[BATTERY] Control Method Battery (PNP0C0A) active: ");
        serial_print_dec(status.capacity_percent);
        serial_puts("%\n");
    } else {
        if (g_battery_present) {
            serial_puts("[BATTERY] Control Method Battery detected (probe pending / EC busy)\n");
        } else {
            serial_puts("[BATTERY] No ACPI battery found (desktop mode)\n");
        }
    }
#else
    if (g_ec_reader) {
        g_battery_present = true;
    }
#endif
}

bool acpi_battery_is_present(void) {
    if (!g_battery_initialized) {
        acpi_battery_init();
    }
    return g_battery_present;
}

int acpi_battery_get(battery_status_t *out) {
    if (!out) return SYSCALL_EFAULT;

#ifdef HOST_TEST
    if (g_ec_reader) {
        g_battery_present = true;
    }
#endif

    if (!g_battery_initialized) {
        acpi_battery_init();
    }

    if (!g_battery_present) {
        return SYSCALL_ENODEV;
    }

    uint64_t now = get_current_ticks();
    uint64_t freq = get_tick_frequency();
    uint64_t cache_period = 5 * freq; /* 5 seconds */

    /* If cached and within 5 seconds, return immediately without touching slow EC */
    if (g_has_cached && (now - g_last_update_ticks) < cache_period) {
        memcpy(out, &g_cached_battery, sizeof(battery_status_t));
        return 0;
    }

    /* Cache expired: refresh from hardware/EC */
    battery_status_t refreshed;
    bool ok = poll_hardware_battery(&refreshed);

    if (!ok) {
        memset(&g_cached_battery, 0, sizeof(g_cached_battery));
        g_has_cached = false;
        return SYSCALL_EIO;
    }

    g_cached_battery = refreshed;
    g_last_update_ticks = now;
    g_has_cached = true;

    memcpy(out, &g_cached_battery, sizeof(battery_status_t));
    return 0;
}
