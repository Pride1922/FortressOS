#define HOST_TEST 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "acpi_battery.h"
#include "syscall_abi.h"

extern void test_set_mock_ticks(uint64_t ticks);
extern void test_add_mock_ticks(uint64_t delta);

/* Mock EC Data Store */
static uint8_t s_mock_ec_regs[256];
static int s_mock_ec_read_count = 0;
static bool s_mock_ec_fail;

static int mock_ec_reader(uint8_t addr, uint8_t *val) {
    s_mock_ec_read_count++;
    if (s_mock_ec_fail) return -1;
    *val = s_mock_ec_regs[addr];
    return 0;
}

static void test_percentage_calculation(void) {
    puts("[TEST 1] Battery Percentage Calculation & Clamping...");
    battery_status_t st;

    /* 1. Normal: 85% */
    acpi_battery_compute_status(0x02 /* Charging */, 1500, 4250, 11400, 5000, &st);
    assert(st.capacity_percent == 85);
    assert(st.state == BATTERY_STATE_CHARGING);

    /* 2. Clamping to 100 when remaining > full */
    acpi_battery_compute_status(0x02, 1000, 5200, 12000, 5000, &st);
    assert(st.capacity_percent == 100);

    /* 3. Zero full charge capacity (no division by zero) */
    acpi_battery_compute_status(0x01, 1000, 2000, 11000, 0, &st);
    assert(st.capacity_percent == 0);

    /* 4. Zero remaining capacity */
    acpi_battery_compute_status(0x01, 1000, 0, 10000, 5000, &st);
    assert(st.capacity_percent == 0);

    puts("       [PASS] Percentage calculation and clamping verified");
}

static void test_time_remaining(void) {
    puts("[TEST 2] Time Remaining Calculation & Sane Maximum Clamping...");
    battery_status_t st;

    /* 1. Normal Discharging: 4500 mAh / 2000 mA -> 135 minutes (2h 15m) */
    acpi_battery_compute_status(0x01 /* Discharging */, 2000, 4500, 11400, 5000, &st);
    assert(st.state == BATTERY_STATE_DISCHARGING);
    assert(st.time_remaining_mins == 135);

    /* 2. PresentRate = 0 (no division by zero) */
    acpi_battery_compute_status(0x01, 0, 4500, 11400, 5000, &st);
    assert(st.time_remaining_mins == 0);

    /* 3. Extremely low rate -> clamps to 9999 max */
    acpi_battery_compute_status(0x01, 1, 4500, 11400, 5000, &st);
    assert(st.time_remaining_mins == 9999);

    /* 4. Charging state -> time remaining must be 0 */
    acpi_battery_compute_status(0x02 /* Charging */, 2000, 4500, 11400, 5000, &st);
    assert(st.state == BATTERY_STATE_CHARGING);
    assert(st.time_remaining_mins == 0);

    /* 5. Full state -> time remaining must be 0 */
    acpi_battery_compute_status(0x00, 0, 5000, 12600, 5000, &st);
    assert(st.state == BATTERY_STATE_FULL);
    assert(st.time_remaining_mins == 0);

    puts("       [PASS] Time remaining calculation and 9999m max clamp verified");
}

static void test_state_mapping(void) {
    puts("[TEST 3] Battery State Mapping...");
    battery_status_t st;

    /* Charging (bit 1) */
    acpi_battery_compute_status(0x02, 1000, 2000, 11000, 5000, &st);
    assert(st.state == BATTERY_STATE_CHARGING);

    /* Discharging (bit 0) */
    acpi_battery_compute_status(0x01, 1000, 2000, 11000, 5000, &st);
    assert(st.state == BATTERY_STATE_DISCHARGING);

    /* Critical discharging (bit 2 + bit 0) */
    acpi_battery_compute_status(0x05, 1000, 200, 10000, 5000, &st);
    assert(st.state == BATTERY_STATE_DISCHARGING);

    /* Full capacity reached (state 0 + 100%) */
    acpi_battery_compute_status(0x00, 0, 5000, 12600, 5000, &st);
    assert(st.state == BATTERY_STATE_FULL);

    /* Near-full capacity does not prove full charge or AC presence. */
    acpi_battery_compute_status(0x00, 0, 4800, 12600, 5000, &st);
    assert(st.state == BATTERY_STATE_UNKNOWN);
    assert(st.ac_online == AC_POWER_UNKNOWN);
    acpi_battery_compute_status(0x00, 0, 5000, 12600, 5000, &st);
    assert(st.state == BATTERY_STATE_FULL && st.ac_online == AC_POWER_UNKNOWN);

    /* Unknown / disconnected */
    acpi_battery_compute_status(0x00, 0, 0, 0, 0, &st);
    assert(st.state == BATTERY_STATE_UNKNOWN);

    puts("       [PASS] State mapping verified");
}

static void test_aml_parsing(void) {
    puts("[TEST 4] AML Device Discovery & Package Evaluation (_BST, _BIX, _BIF)...");

    /* Synthetic AML buffer containing PNP0C0A device, _BST, and _BIX */
    uint8_t aml_fixture[] = {
        /* PNP0C0A string */
        'P', 'N', 'P', '0', 'C', '0', 'A', 0x00,
        /* _BST object */
        '_', 'B', 'S', 'T',
        0x12, 0x0C, 0x04,           /* PackageOp, length 12, 4 elements */
        0x0A, 0x02,                 /* Element 0: State = 2 (Charging) */
        0x0B, 0xDC, 0x05,           /* Element 1: PresentRate = 1500 (Word: 0x05DC) */
        0x0B, 0x9A, 0x10,           /* Element 2: RemainingCapacity = 4250 (Word: 0x109A) */
        0x0B, 0x88, 0x2C,           /* Element 3: PresentVoltage = 11400 (Word: 0x2C88) */

        /* _BIX object */
        '_', 'B', 'I', 'X',
        0x12, 0x10, 0x04,           /* PackageOp, length 16, 4 elements */
        0x0A, 0x00,                 /* Revision = 0 */
        0x0A, 0x00,                 /* PowerUnit = 0 */
        0x0B, 0x88, 0x13,           /* DesignCapacity = 5000 */
        0x0B, 0x88, 0x13            /* LastFullChargeCapacity = 5000 (Word: 0x1388) */
    };

    size_t dev_offset = 0;
    assert(acpi_battery_locate(aml_fixture, sizeof(aml_fixture), &dev_offset));

    uint32_t bst_state = 0, rate = 0, rem_cap = 0, voltage = 0;
    assert(acpi_battery_evaluate_bst(aml_fixture, sizeof(aml_fixture), &bst_state, &rate, &rem_cap, &voltage));
    assert(bst_state == 2);
    assert(rate == 1500);
    assert(rem_cap == 4250);
    assert(voltage == 11400);

    uint32_t last_full = 0;
    assert(acpi_battery_evaluate_bix(aml_fixture, sizeof(aml_fixture), &last_full));
    assert(last_full == 5000);

    /* Compute status from parsed AML values */
    battery_status_t st;
    acpi_battery_compute_status(bst_state, rate, rem_cap, voltage, last_full, &st);
    assert(st.capacity_percent == 85);
    assert(st.state == BATTERY_STATE_CHARGING);
    assert(st.time_remaining_mins == 0);
    assert(st.voltage_mv == 11400);

    /* Test fallback to _BIF */
    uint8_t bif_fixture[] = {
        '_', 'B', 'I', 'F',
        0x12, 0x0E, 0x03,           /* PackageOp, 3 elements */
        0x0A, 0x00,                 /* PowerUnit = 0 */
        0x0B, 0x88, 0x13,           /* DesignCapacity = 5000 */
        0x0B, 0x20, 0x12            /* LastFullChargeCapacity = 4640 (0x1220) */
    };
    uint32_t bif_full = 0;
    assert(acpi_battery_evaluate_bif(bif_fixture, sizeof(bif_fixture), &bif_full));
    assert(bif_full == 4640);

    puts("       [PASS] AML device discovery, _BST, _BIX, and _BIF parsers verified");
}

static void test_mock_ec_and_caching(void) {
    puts("[TEST 5] Mock ACPI EC & 5-Second Cache Verification...");
    acpi_battery_reset();

    /* Populate synthetic host-only registers (not a Dell hardware map):
     * 85% Charging: Full = 5000 (0x1388), Remaining = 4250 (0x109A), Rate = 1500 (0x05DC) */
    memset(s_mock_ec_regs, 0, sizeof(s_mock_ec_regs));
    s_mock_ec_regs[0x10] = 0x02; /* State: Charging */
    s_mock_ec_regs[0x11] = 0x9A; /* RemCap low */
    s_mock_ec_regs[0x12] = 0x10; /* RemCap high */
    s_mock_ec_regs[0x13] = 0xDC; /* Rate low */
    s_mock_ec_regs[0x14] = 0x05; /* Rate high */
    s_mock_ec_regs[0x15] = 0x88; /* FullCap low */
    s_mock_ec_regs[0x16] = 0x13; /* FullCap high */
    s_mock_ec_regs[0x17] = 0x88; /* Volt low */
    s_mock_ec_regs[0x18] = 0x2C; /* Volt high (11400 mV) */

    acpi_battery_set_ec_reader(mock_ec_reader);
    s_mock_ec_read_count = 0;
    test_set_mock_ticks(1000);

    /* Initialize and simulate battery present */
    acpi_battery_init();
    /* Force battery present for EC mock test */
    acpi_battery_force_update();

    battery_status_t st;
    /* First call should query EC */
    int res = acpi_battery_get(&st);
    assert(res == 0);
    assert(st.capacity_percent == 85);
    assert(st.state == BATTERY_STATE_CHARGING);
    assert(st.voltage_mv == 11400);
    assert(s_mock_ec_read_count > 0);
    int read_count_after_first = s_mock_ec_read_count;

    /* Second call after 2 seconds (< 5 seconds): must use cache, zero additional EC reads! */
    test_add_mock_ticks(2000);
    res = acpi_battery_get(&st);
    assert(res == 0);
    assert(st.capacity_percent == 85);
    assert(s_mock_ec_read_count == read_count_after_first); /* No slow EC reads! */

    /* Third call after 3 more seconds (total 5 seconds): cache expires, re-reads EC */
    test_add_mock_ticks(3001);
    /* Change mock EC to Discharging, 80% (Rem = 4000 = 0x0FA0, Rate = 2000 = 0x07D0) */
    s_mock_ec_regs[0x10] = 0x01; /* Discharging */
    s_mock_ec_regs[0x11] = 0xA0;
    s_mock_ec_regs[0x12] = 0x0F;
    s_mock_ec_regs[0x13] = 0xD0;
    s_mock_ec_regs[0x14] = 0x07;

    res = acpi_battery_get(&st);
    assert(res == 0);
    assert(st.capacity_percent == 80);
    assert(st.state == BATTERY_STATE_DISCHARGING);
    /* (4000 * 60) / 2000 = 120 minutes (2h 00m) */
    assert(st.time_remaining_mins == 120);
    assert(s_mock_ec_read_count > read_count_after_first);

    puts("       [PASS] Mock ACPI EC & 5-second cache validated");
}

static void test_unavailable_refresh(void) {
    acpi_battery_reset();
    acpi_battery_set_ec_reader(mock_ec_reader);
    s_mock_ec_fail=false;
    battery_status_t st;
    assert(!acpi_battery_get(&st));
    test_add_mock_ticks(5001);
    s_mock_ec_fail=true;
    memset(&st,0xa5,sizeof(st));
    battery_status_t before=st;
    assert(acpi_battery_get(&st)==SYSCALL_EIO);
    assert(!memcmp(&st,&before,sizeof(st)));
    /* Failed refresh must also invalidate the cache for immediate retry. */
    s_mock_ec_fail=false;
    assert(!acpi_battery_get(&st));
    puts("       [PASS] Failed telemetry refresh never returns stale success");
}

static void test_ac_and_thermal(void) {
    puts("[TEST 6] AC Adapter (_PSR) & Thermal Zone (_TMP) Evaluation...");

    /* 1. AML fixture with _PSR (Power Source = 1: Online) */
    uint8_t psr_online_fixture[] = {
        '_', 'P', 'S', 'R',
        0xA4, 0x01 /* ReturnOp, OneOp */
    };
    uint32_t online = 0;
    assert(acpi_ac_adapter_evaluate_psr(psr_online_fixture, sizeof(psr_online_fixture), &online));
    assert(online == AC_POWER_ONLINE);

    /* 2. AML fixture with _PSR (Power Source = 0: Offline) */
    uint8_t psr_offline_fixture[] = {
        '_', 'P', 'S', 'R',
        0xA4, 0x00 /* ReturnOp, ZeroOp */
    };
    assert(acpi_ac_adapter_evaluate_psr(psr_offline_fixture, sizeof(psr_offline_fixture), &online));
    assert(online == AC_POWER_OFFLINE);

    /* 3. AML fixture with _TMP (Temperature = 3000 dK -> 26°C) */
    uint8_t tmp_fixture[] = {
        '_', 'T', 'M', 'P',
        0xA4, 0x0B, 0xB8, 0x0B /* ReturnOp, WordPrefix: 0x0BB8 = 3000 dK */
    };
    int32_t temp_c = 0;
    assert(acpi_thermal_evaluate_tmp(tmp_fixture, sizeof(tmp_fixture), &temp_c));
    assert(temp_c == 26); /* (3000 - 2732) / 10 = 26 */

    /* 4. Thermal Zone at 50°C (3232 dK = 0x0CA0) */
    uint8_t tmp_50_fixture[] = {
        '_', 'T', 'M', 'P',
        0xA4, 0x0B, 0xA0, 0x0C /* ReturnOp, WordPrefix: 3232 dK */
    };
    assert(acpi_thermal_evaluate_tmp(tmp_50_fixture, sizeof(tmp_50_fixture), &temp_c));
    assert(temp_c == 50); /* (3232 - 2732) / 10 = 50 */

    /* 5. Status computation integrates AC online and thermal */
    battery_status_t st;
    acpi_battery_reset();
    acpi_thermal_set_mock(45);
    acpi_ac_adapter_set_mock(AC_POWER_ONLINE);
    acpi_battery_compute_status(0x02 /* Charging */, 1500, 4250, 11400, 5000, &st);
    assert(st.ac_online == AC_POWER_ONLINE);
    assert(st.temperature_c == 45);

    puts("       [PASS] AC adapter and thermal zone evaluation verified");
}

static void test_ec_field_parsing_and_status(void) {
    puts("[TEST 7] AML EC Field Decoding & Dynamic EC Status...");
    uint8_t ec_field_aml[] = {
        0x5B, 0x81, 0x25, /* FieldOp, PkgLength = 37 */
        'E', 'C', 'O', 'R', /* RegionName */
        0x01, /* FieldFlags */
        0x00, 0x40, 0x08, /* ReservedField (Offset 0x10 = 128 bits) */
        'B', '1', 'S', 'T', 0x08, /* B1ST, 8 bits */
        'B', '1', 'P', 'R', 0x10, /* B1PR, 16 bits */
        'B', '1', 'R', 'C', 0x10, /* B1RC, 16 bits */
        'B', '1', 'P', 'V', 0x10, /* B1PV, 16 bits */
        0x00, 0x48, 0x0C, /* ReservedField (skip 200 bits to 0x30) */
        'B', '1', 'F', 'C', 0x10  /* B1FC, 16 bits */
    };

    acpi_ec_battery_map_t map;
    memset(&map, 0, sizeof(map));
    assert(acpi_parse_ec_fields(ec_field_aml, sizeof(ec_field_aml), &map));
    assert(map.found_rem && map.found_full && map.found_state && map.found_rate && map.found_volt);
    assert(map.state_offset == 0x10 && map.state_bits == 8);
    assert(map.rate_offset == 0x11 && map.rate_bits == 16);
    assert(map.rem_cap_offset == 0x13 && map.rem_cap_bits == 16);
    assert(map.volt_offset == 0x15 && map.volt_bits == 16);
    assert(map.full_cap_offset == 0x30 && map.full_cap_bits == 16);

    /* Test reading via dynamic EC map */
    memset(s_mock_ec_regs, 0, sizeof(s_mock_ec_regs));
    s_mock_ec_regs[0x10] = 0x02; /* Charging */
    s_mock_ec_regs[0x11] = 0xDC; /* Rate = 1500 */
    s_mock_ec_regs[0x12] = 0x05;
    s_mock_ec_regs[0x13] = 0x9A; /* Rem = 4250 */
    s_mock_ec_regs[0x14] = 0x10;
    s_mock_ec_regs[0x15] = 0x88; /* Volt = 11400 */
    s_mock_ec_regs[0x16] = 0x2C;
    s_mock_ec_regs[0x30] = 0x88; /* Full = 5000 */
    s_mock_ec_regs[0x31] = 0x13;

    acpi_battery_set_ec_reader(mock_ec_reader);
    battery_status_t st;
    memset(&st, 0, sizeof(st));
    assert(acpi_read_ec_battery(&map, &st));
    assert(st.capacity_percent == 85);
    assert(st.state == BATTERY_STATE_CHARGING);
    assert(st.voltage_mv == 11400);

    puts("       [PASS] Dynamic AML EC Field parsing and EC status verified");
}

static void test_dell_ec_field_parsing_and_ecdt(void) {
    puts("[TEST 8] Dell Scoped EC Field Parsing, 16-bit State & ECDT Validation...");

    /* 1. Test ECDT parser with valid System I/O ports */
    uint8_t valid_ecdt[68];
    memset(valid_ecdt, 0, sizeof(valid_ecdt));
    memcpy(valid_ecdt, "ECDT", 4);
    uint32_t len = sizeof(valid_ecdt);
    memcpy(valid_ecdt + 4, &len, 4);
    /* ec_control GAS at offset 36: space 1 (I/O), port 0x6C */
    valid_ecdt[36] = 1;
    uint64_t ctrl_port = 0x6C;
    memcpy(valid_ecdt + 40, &ctrl_port, 8);
    /* ec_data GAS at offset 48: space 1 (I/O), port 0x68 */
    valid_ecdt[48] = 1;
    uint64_t data_port = 0x68;
    memcpy(valid_ecdt + 52, &data_port, 8);

    uint16_t parsed_cmd = 0, parsed_data = 0;
    assert(acpi_parse_ecdt(valid_ecdt, sizeof(valid_ecdt), &parsed_cmd, &parsed_data));
    assert(parsed_cmd == 0x6C);
    assert(parsed_data == 0x68);

    /* Test ECDT rejection of MMIO space (space 0) */
    valid_ecdt[36] = 0; /* MMIO */
    assert(!acpi_parse_ecdt(valid_ecdt, sizeof(valid_ecdt), &parsed_cmd, &parsed_data));

    /* Test ECDT rejection of bad signature */
    memcpy(valid_ecdt, "FACP", 4);
    valid_ecdt[36] = 1;
    assert(!acpi_parse_ecdt(valid_ecdt, sizeof(valid_ecdt), &parsed_cmd, &parsed_data));

    /* 2. Test port setters, getters, and reset isolation */
    acpi_battery_reset();
    uint16_t d_port = 0, c_port = 0;
    acpi_battery_get_ec_ports(&d_port, &c_port);
    assert(d_port == 0x62 && c_port == 0x66);

    acpi_battery_set_ec_ports(0x68, 0x6C);
    acpi_battery_get_ec_ports(&d_port, &c_port);
    assert(d_port == 0x68 && c_port == 0x6C);

    acpi_battery_reset();
    acpi_battery_get_ec_ports(&d_port, &c_port);
    assert(d_port == 0x62 && c_port == 0x66);

    /* 3. Dell DSDT Fixture with DualNamePath (0x2E: \_SB.PCI0.LPCB.ECDV / \EC0_.ECOR) and Dell field names */
    uint8_t dell_dual_aml[] = {
        0x5B, 0x81, 0x28,                   /* FieldOp, PkgLength = 40 */
        0x5C, 0x2E, 'E', 'C', '0', '_', 'E', 'C', 'O', 'R', /* RegionName: \EC0_.ECOR (DualNamePath) */
        0x01,                               /* FieldFlags */
        0x00, 0x40, 0x10,                   /* ReservedField: skip 256 bits (offset 0x20 = 32 bytes) */
        'M', 'B', 'S', 'T', 0x10,           /* MBST (Dell Main Battery State, 16-bit) */
        'M', 'B', 'P', 'R', 0x10,           /* MBPR (Dell Present Rate, 16-bit) */
        'M', 'B', 'R', 'M', 0x10,           /* MBRM (Dell Remaining Capacity, 16-bit) */
        'M', 'B', 'V', 'O', 0x10,           /* MBVO (Dell Present Voltage, 16-bit) */
        'M', 'B', 'F', 'C', 0x10            /* MBFC (Dell Full Capacity, 16-bit) */
    };

    acpi_ec_battery_map_t map;
    memset(&map, 0, sizeof(map));
    assert(acpi_parse_ec_fields(dell_dual_aml, sizeof(dell_dual_aml), &map));
    assert(map.found_state && map.found_rate && map.found_rem && map.found_volt && map.found_full);
    assert(map.state_offset == 0x20 && map.state_bits == 16);
    assert(map.rate_offset == 0x22 && map.rate_bits == 16);
    assert(map.rem_cap_offset == 0x24 && map.rem_cap_bits == 16);
    assert(map.volt_offset == 0x26 && map.volt_bits == 16);
    assert(map.full_cap_offset == 0x28 && map.full_cap_bits == 16);

    /* 4. Test live reading of Dell EC mapping with 16-bit state */
    memset(s_mock_ec_regs, 0, sizeof(s_mock_ec_regs));
    /* MBST: 0x0001 = Discharging */
    s_mock_ec_regs[0x20] = 0x01;
    s_mock_ec_regs[0x21] = 0x00;
    /* MBPR: 1200 mA (0x04B0) */
    s_mock_ec_regs[0x22] = 0xB0;
    s_mock_ec_regs[0x23] = 0x04;
    /* MBRM: 3600 mAh (0x0E10) */
    s_mock_ec_regs[0x24] = 0x10;
    s_mock_ec_regs[0x25] = 0x0E;
    /* MBVO: 11400 mV (0x2C88) */
    s_mock_ec_regs[0x26] = 0x88;
    s_mock_ec_regs[0x27] = 0x2C;
    /* MBFC: 4800 mAh (0x12C0) */
    s_mock_ec_regs[0x28] = 0xC0;
    s_mock_ec_regs[0x29] = 0x12;

    acpi_battery_set_ec_reader(mock_ec_reader);
    battery_status_t st;
    memset(&st, 0, sizeof(st));
    assert(acpi_read_ec_battery(&map, &st));
    /* 3600 / 4800 = 75% */
    assert(st.capacity_percent == 75);
    assert(st.state == BATTERY_STATE_DISCHARGING);
    /* (3600 * 60) / 1200 = 180 mins (3h) */
    assert(st.time_remaining_mins == 180);
    assert(st.voltage_mv == 11400);

    /* 5. Dell MultiNamePath Fixture (0x2F: 3 segments) */
    uint8_t dell_multi_aml[] = {
        0x5B, 0x81, 0x1A,                   /* FieldOp, PkgLength = 26 */
        0x2F, 0x03,                         /* MultiNamePrefix, 3 segments */
        'L', 'P', 'C', 'B',
        'E', 'C', 'D', 'V',
        'E', 'C', 'O', 'R',
        0x01,                               /* FieldFlags */
        'M', 'B', 'R', 'M', 0x10,           /* MBRM, 16-bit at offset 0 */
        'M', 'B', 'F', 'C', 0x10            /* MBFC, 16-bit at offset 2 */
    };
    memset(&map, 0, sizeof(map));
    assert(acpi_parse_ec_fields(dell_multi_aml, sizeof(dell_multi_aml), &map));
    assert(map.found_rem && map.found_full);
    assert(map.rem_cap_offset == 0 && map.full_cap_offset == 2);

    puts("       [PASS] Dell scoped EC fields, 16-bit state, and ECDT parser verified");
}

static void test_dell_cap0_fcp0_fields(void) {
    puts("[TEST 9] Dell Latitude CAP0/FCP0/CRT0/VOT0/BST0 Field Parsing & Status...");

    /* Dell Latitude / XPS AML FieldOp with CAP0 (rem), VOT0 (volt), CRT0 (rate), FCP0 (full), BST0 (state) */
    uint8_t dell_cap0_aml[] = {
        0x5B, 0x81, 0x25,                   /* FieldOp, PkgLength = 37 */
        0x5C, 0x2E, 'E', 'C', '0', '_', 'E', 'C', 'O', 'R', /* RegionName: \EC0_.ECOR */
        0x01,                               /* FieldFlags */
        'C', 'A', 'P', '0', 0x10,           /* CAP0: 16-bit Remaining Capacity at offset 0 */
        'V', 'O', 'T', '0', 0x10,           /* VOT0: 16-bit Present Voltage at offset 2 */
        'C', 'R', 'T', '0', 0x10,           /* CRT0: 16-bit Present Rate at offset 4 */
        'F', 'C', 'P', '0', 0x10,           /* FCP0: 16-bit Full Charge Capacity at offset 6 */
        'B', 'S', 'T', '0', 0x10            /* BST0: 16-bit Battery State at offset 8 */
    };

    acpi_ec_battery_map_t map;
    memset(&map, 0, sizeof(map));
    assert(acpi_parse_ec_fields(dell_cap0_aml, sizeof(dell_cap0_aml), &map));
    assert(map.found_rem && map.found_full && map.found_rate && map.found_volt && map.found_state);
    assert(map.rem_cap_offset == 0 && map.rem_cap_bits == 16);
    assert(map.volt_offset == 2 && map.volt_bits == 16);
    assert(map.rate_offset == 4 && map.rate_bits == 16);
    assert(map.full_cap_offset == 6 && map.full_cap_bits == 16);
    assert(map.state_offset == 8 && map.state_bits == 16);

    /* Test live EC reading with these offsets */
    memset(s_mock_ec_regs, 0, sizeof(s_mock_ec_regs));
    /* CAP0: 3000 mAh = 0x0BB8 */
    s_mock_ec_regs[0] = 0xB8; s_mock_ec_regs[1] = 0x0B;
    /* VOT0: 12000 mV = 0x2EE0 */
    s_mock_ec_regs[2] = 0xE0; s_mock_ec_regs[3] = 0x2E;
    /* CRT0: 1500 mA = 0x05DC */
    s_mock_ec_regs[4] = 0xDC; s_mock_ec_regs[5] = 0x05;
    /* FCP0: 6000 mAh = 0x1770 */
    s_mock_ec_regs[6] = 0x70; s_mock_ec_regs[7] = 0x17;
    /* BST0: 0x0001 (Discharging) */
    s_mock_ec_regs[8] = 0x01; s_mock_ec_regs[9] = 0x00;

    acpi_battery_set_ec_reader(mock_ec_reader);
    battery_status_t st;
    memset(&st, 0, sizeof(st));
    assert(acpi_read_ec_battery(&map, &st));
    assert(st.capacity_percent == 50); /* 3000 / 6000 = 50% */
    assert(st.state == BATTERY_STATE_DISCHARGING);
    assert(st.time_remaining_mins == 120); /* (3000 * 60) / 1500 = 120 */
    assert(st.voltage_mv == 12000);

    /* Test fallback state with AC online */
    s_mock_ec_regs[8] = 0x00; /* EC reports state 0 (unknown) */
    acpi_ac_adapter_set_mock(AC_POWER_ONLINE);
    assert(acpi_read_ec_battery(&map, &st));
    assert(st.state == BATTERY_STATE_CHARGING);

    /* Test fallback state with AC online and >=98% capacity */
    s_mock_ec_regs[0] = 0x70; s_mock_ec_regs[1] = 0x17; /* 6000 / 6000 = 100% */
    assert(acpi_read_ec_battery(&map, &st));
    assert(st.capacity_percent == 100);
    assert(st.state == BATTERY_STATE_FULL);

    puts("       [PASS] Dell Latitude CAP0/FCP0/CRT0/VOT0/BST0 and state fallback verified");
}

int main(void) {
    puts("=== Running FortressOS ACPI Battery Verification Suite ===");
    test_percentage_calculation();
    test_time_remaining();
    test_state_mapping();
    test_aml_parsing();
    test_mock_ec_and_caching();
    test_unavailable_refresh();
    test_ac_and_thermal();
    test_ec_field_parsing_and_status();
    test_dell_ec_field_parsing_and_ecdt();
    test_dell_cap0_fcp0_fields();
    puts("=== All Battery Tests Passed (100%) ===");
    return 0;
}
