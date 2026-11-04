#ifndef FORTRESS_ACPI_BATTERY_H
#define FORTRESS_ACPI_BATTERY_H

#include "types.h"
#include "syscall_abi.h"

/* Standard ACPI Embedded Controller Ports */
#define ACPI_EC_DATA_PORT       0x62
#define ACPI_EC_CMD_PORT        0x66
#define ACPI_EC_STATUS_PORT     0x66

/* Dell Latitude / Precision / XPS Embedded Controller Ports */
#define DELL_EC_DATA_PORT       0x0930
#define DELL_EC_CMD_PORT        0x0934
#define DELL_EC_STATUS_PORT     0x0934

/* EC Status Register Bits */
#define ACPI_EC_FLAG_OBF        0x01 /* Output Buffer Full (data ready to read) */
#define ACPI_EC_FLAG_IBF        0x02 /* Input Buffer Full (EC busy processing input) */
#define ACPI_EC_FLAG_CMD        0x04 /* Command/Data byte */
#define ACPI_EC_FLAG_BURST      0x08 /* Burst mode active */
#define ACPI_EC_FLAG_SCI_EVT    0x10 /* SCI event pending */
#define ACPI_EC_FLAG_SMI_EVT    0x20 /* SMI event pending */

/* EC Standard Commands */
#define ACPI_EC_CMD_RD          0x80 /* Read Embedded Controller */
#define ACPI_EC_CMD_WR          0x81 /* Write Embedded Controller */
#define ACPI_EC_CMD_BE          0x82 /* Burst Enable */
#define ACPI_EC_CMD_BD          0x83 /* Burst Disable */
#define ACPI_EC_CMD_QR          0x84 /* Query Embedded Controller */

/* ACPI Battery Public API */
void acpi_battery_init(void);
int  acpi_battery_get(battery_status_t *out);
bool acpi_battery_is_present(void);
void acpi_battery_set_ec_ports(uint16_t data_port, uint16_t cmd_port);
void acpi_battery_get_ec_ports(uint16_t *data_port, uint16_t *cmd_port);

/* Dynamic EC Field Mapping extracted from DSDT/SSDT OperationRegions */
typedef struct {
    uint8_t state_offset;
    uint8_t state_bits;
    uint8_t rem_cap_offset;
    uint8_t rem_cap_bits;
    uint8_t full_cap_offset;
    uint8_t full_cap_bits;
    uint8_t rate_offset;
    uint8_t rate_bits;
    uint8_t volt_offset;
    uint8_t volt_bits;
    uint8_t design_cap_offset;
    uint8_t design_cap_bits;
    bool found_state;
    bool found_rem;
    bool found_full;
    bool found_design;
    bool found_rate;
    bool found_volt;
} acpi_ec_battery_map_t;

bool acpi_parse_ec_fields(const uint8_t *aml, size_t len, acpi_ec_battery_map_t *map);
bool acpi_read_ec_battery(const acpi_ec_battery_map_t *map, battery_status_t *out);
bool acpi_parse_ecdt(const uint8_t *table, size_t len, uint16_t *cmd_port, uint16_t *data_port);
bool acpi_parse_dsdt_ec_ports(const uint8_t *aml, size_t len, uint16_t *cmd_port, uint16_t *data_port);
bool acpi_read_dell_ec_battery(battery_status_t *out);

/* ACPI AML Parser and Evaluation Primitives */
bool acpi_battery_locate(const uint8_t *aml, size_t len, size_t *out_offset);
bool acpi_battery_parse_package(const uint8_t *ptr, const uint8_t *end, uint32_t *elements, size_t max_elements, size_t *num_found);
bool acpi_battery_evaluate_bst(const uint8_t *aml, size_t len, uint32_t *state, uint32_t *rate, uint32_t *rem_cap, uint32_t *voltage);
bool acpi_battery_evaluate_bix(const uint8_t *aml, size_t len, uint32_t *last_full_cap);
bool acpi_battery_evaluate_bif(const uint8_t *aml, size_t len, uint32_t *last_full_cap);
bool acpi_ac_adapter_evaluate_psr(const uint8_t *aml, size_t len, uint32_t *online);
bool acpi_thermal_evaluate_tmp(const uint8_t *aml, size_t len, int32_t *temp_c);
void acpi_battery_compute_status(uint32_t bst_state, uint32_t present_rate, uint32_t rem_cap, uint32_t voltage, uint32_t full_cap, battery_status_t *out);

/* EC Interface & Test Mock Hooks */
typedef int (*acpi_ec_read_fn_t)(uint8_t addr, uint8_t *val);
int  acpi_ec_read(uint8_t addr, uint8_t *val);
int  acpi_ec_write(uint8_t addr, uint8_t val);
void acpi_battery_set_ec_reader(acpi_ec_read_fn_t reader);
void acpi_thermal_set_mock(int32_t temp_c);
void acpi_ac_adapter_set_mock(uint32_t online);
void acpi_battery_force_update(void);
void acpi_battery_reset(void);

#endif /* FORTRESS_ACPI_BATTERY_H */
