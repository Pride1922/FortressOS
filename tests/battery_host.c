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

static int mock_ec_reader(uint8_t addr, uint8_t *val) {
    s_mock_ec_read_count++;
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

    /* Near full AC connected (state 0 + >= 95%) */
    acpi_battery_compute_status(0x00, 0, 4800, 12600, 5000, &st);
    assert(st.state == BATTERY_STATE_FULL);

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

    /* Populate Mock EC registers for Dell 5590 test values:
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

int main(void) {
    puts("=== Running FortressOS ACPI Battery Verification Suite ===");
    test_percentage_calculation();
    test_time_remaining();
    test_state_mapping();
    test_aml_parsing();
    test_mock_ec_and_caching();
    puts("=== All Battery Tests Passed (100%) ===");
    return 0;
}
