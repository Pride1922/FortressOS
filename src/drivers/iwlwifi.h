#ifndef FORTRESS_IWLWIFI_H
#define FORTRESS_IWLWIFI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "spinlock.h"
#include "pci.h"
#include "net.h"

#define IWL_PCI_VENDOR_ID               0x8086

/* Supported Intel Wireless Device IDs */
#define IWL_DEV_ID_AC_8265              0x24FD  /* Dell Latitude 5590 */
#define IWL_DEV_ID_AC_8260              0x24F3
#define IWL_DEV_ID_AC_7265              0x095A
#define IWL_DEV_ID_AC_3165              0x3165
#define IWL_DEV_ID_AC_9260              0x2526
#define IWL_DEV_ID_AX_200               0x2723
#define IWL_DEV_ID_AX_201               0x43F0
#define IWL_DEV_ID_AX_210               0x2725
#define IWL_DEV_ID_AX_211               0x51F0  /* Dell Latitude 5530 */

/* Core CSR Registers */
#define CSR_HW_IF_CONFIG_REG            0x000
#define CSR_INT_COALESCING              0x004
#define CSR_INT                         0x008
#define CSR_INT_MASK                    0x00C
#define CSR_FH_INT_STATUS               0x010
#define CSR_GPIO_IN                     0x018
#define CSR_RESET                       0x020
#define CSR_GP_CNTRL                    0x024
#define CSR_HW_REV                      0x028
#define CSR_EEPROM_REG                  0x02C
#define CSR_OTP_GP_REG                  0x034
#define CSR_GIO_REG                     0x03C
#define CSR_UCODE_DRV_GP1_CLR           0x05C
#define CSR_MBOX_SET_REG                0x088
#define CSR_MAC_SHADOW_REG_CTRL         0x0A8
#define CSR_GIO_CHICKEN_BITS            0x100
#define CSR_DBG_LINK_PWR_MGMT_REG       0x250
#define CSR_DBG_HPET_MEM_REG            0x240

/* CSR_RESET Flags */
#define CSR_RESET_REG_FLAG_SW_RESET     0x00000080u
#define CSR_RESET_REG_FLAG_MASTER_DIS   0x00000100u
#define CSR_RESET_REG_FLAG_STOP_MASTER  0x00000200u

/* CSR_GP_CNTRL Flags */
#define CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY 0x00000001u
#define CSR_GP_CNTRL_REG_FLAG_INIT_DONE       0x00000004u
#define CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ  0x00000008u
#define CSR_GP_CNTRL_REG_FLAG_GOING_TO_SLEEP  0x00000010u
#define CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW   0x08000000u

/* Interrupt bits */
#define CSR_INT_BIT_ALIVE               (1u << 0)
#define CSR_INT_BIT_WAKEUP              (1u << 1)
#define CSR_INT_BIT_SW_RX               (1u << 3)
#define CSR_INT_BIT_RF_KILL             (1u << 7)
#define CSR_INT_BIT_SW_ERR              (1u << 25)
#define CSR_INT_BIT_SCD                 (1u << 26)
#define CSR_INT_BIT_FH_TX               (1u << 27)
#define CSR_INT_BIT_HW_ERR              (1u << 29)
#define CSR_INT_BIT_FH_RX               (1u << 31)

#define CSR_INI_SET_MASK                (CSR_INT_BIT_FH_RX | CSR_INT_BIT_HW_ERR | \
                                         CSR_INT_BIT_FH_TX | CSR_INT_BIT_SCD | \
                                         CSR_INT_BIT_SW_ERR | CSR_INT_BIT_RF_KILL | \
                                         CSR_INT_BIT_SW_RX | CSR_INT_BIT_WAKEUP | \
                                         CSR_INT_BIT_ALIVE)

/* HBUS & Peripheral register access (PRPH) */
#define HBUS_TARG_MEM_RADDR             0x40C
#define HBUS_TARG_MEM_WADDR             0x410
#define HBUS_TARG_MEM_WDAT              0x418
#define HBUS_TARG_MEM_RDAT              0x41C
#define HBUS_TARG_PRPH_WADDR            0x444
#define HBUS_TARG_PRPH_RADDR            0x448
#define HBUS_TARG_PRPH_WDAT             0x44C
#define HBUS_TARG_PRPH_RDAT             0x450
#define HBUS_TARG_WRPTR                 0x460

/* Microcode Control & Flow Handler (Family 8000) */
#define IWL_RELEASE_CPU_RESET           0x300C
#define IWL_RELEASE_CPU_RESET_BIT       0x01000000u
#define IWL_WFPM_CTRL_REG               0xA03030u
#define IWL_ENABLE_WFPM                 0x80000000u
#define IWL_WFMP_MAC_ADDR_0             0xA03080u
#define IWL_WFMP_MAC_ADDR_1             0xA03084u
#define IWL_WFPM_GP2                    0xA030B4u
#define IWL_AUX_MISC_REG                0xA200B0u
#define IWL_SB_CPU_1_STATUS             0xA01E30u
#define IWL_SB_CPU_2_STATUS             0xA01E34u
#define IWL_LMPM_CHICK                  0xA01FF8u
#define IWL_LMPM_CHICK_EXTENDED_ADDR_SPACE 0x01u

/* TX Scheduler (SCD) Peripheral Registers */
#define IWL_SCD_BASE                    0xA02C00u
#define IWL_SCD_SRAM_BASE_ADDR          0xA02C00u
#define IWL_SCD_DRAM_BASE_ADDR          0xA02C08u
#define IWL_SCD_AIT                     0xA02C0Cu
#define IWL_SCD_TXFACT                  0xA02C10u
#define IWL_SCD_ACTIVE                  0xA02C14u
#define IWL_SCD_QUEUECHAIN_SEL          0xA02CE8u
#define IWL_SCD_GP_CTRL                 0xA02DA8u
#define IWL_SCD_CHAINEXT_EN             0xA02E44u
#define IWL_SCD_AGGR_SEL                0xA02E48u
#define IWL_SCD_EN_CTRL                 0xA02E54u

#define IWL_SCD_CONTEXT_MEM_LOWER_BOUND 0x600u
#define IWL_SCD_TRANS_TBL_MEM_UPPER_BOUND 0x808u
#define IWL_SCD_CONTEXT_QUEUE_OFFSET(x) (IWL_SCD_CONTEXT_MEM_LOWER_BOUND + ((x) * 8u))

/* Flow Handler (FH) DMA Registers */
#define IWL_FH_SRVC_CHNL                9
#define FH_SRVC_CHNL_SRAM_ADDR_REG(c)   (0x19C8 + (((c) - 9) * 0x4))
#define FH_TFDIB_CTRL0_REG(c)           (0x1900 + (8 * (c)))
#define FH_TFDIB_CTRL1_REG(c)           (0x1900 + (8 * (c)) + 4)
#define FH_KW_MEM_ADDR_REG              0x197C
#define FH_UCODE_LOAD_STATUS            0x1AF0
#define FH_RSCSR_CHNL0_STTS_WPTR_REG    0x1BC0
#define FH_RSCSR_CHNL0_RBDCB_BASE_REG   0x1BC4
#define FH_RSCSR_CHNL0_WPTR             0x1BC8
#define FH_RSCSR_CHNL0_RDPTR            0x1BCC
#define FH_MEM_RCSR_CHNL0_CONFIG_REG    0x1C00
#define FH_MEM_RCSR_CHNL0_RBDCB_WPTR    0x1C08
#define FH_MEM_RCSR_CHNL0_FLUSH_RB_REQ  0x1C10
#define FH_TCSR_CHNL_TX_CONFIG_REG(c)   (0x1D00 + ((c) * 0x20))
#define FH_TCSR_CHNL_TX_BUF_STS_REG(c)  (0x1D00 + ((c) * 0x20) + 8)
#define FH_TX_CHICKEN_BITS_REG          0x1E98
#define FH_TSSR_TX_STATUS_REG           0x1EB0
#define FH_TSSR_TX_ERROR_REG            0x1EB8

#define FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_ENABLE    0x80000000u
#define FH_TCSR_TX_CONFIG_REG_VAL_DMA_CREDIT_DISABLE 0x00000000u
#define FH_TCSR_TX_CONFIG_REG_VAL_DMA_CREDIT_ENABLE  0x00000008u
#define FH_TCSR_TX_CONFIG_REG_VAL_CIRQ_HOST_ENDTFD   0x00100000u

#define CSR_GIO_REG_VAL_L0S_DISABLED    0x00000002u
#define CSR_GIO_CHICKEN_BITS_L1A_NO_L0S_RX 0x00800000u
#define CSR_DBG_HPET_MEM_REG_VAL        0xFFFF0000u
#define CSR_HW_IF_CONFIG_REG_BIT_HAP_WAKE_L1A 0x00080000u
#define CSR_HW_IF_CONFIG_REG_BIT_NIC_READY    0x00400000u
#define CSR_HW_IF_CONFIG_REG_PREPARE          0x08000000u
#define CSR_MBOX_SET_REG_OS_ALIVE             0x00000020u

#define IWL_FW_MEM_EXTENDED_START       0x40000u
#define IWL_FW_MEM_EXTENDED_END         0x57FFFu

/* Firmware Microcode TLV format */
#define IWL_UCODE_TLV_MAGIC             0x0a4c5749u /* "IWL\n" */
#define IWL_UCODE_TLV_FLAGS             18
#define IWL_UCODE_TLV_SEC_RT            19
#define IWL_UCODE_TLV_SEC_INIT          20
#define IWL_UCODE_TLV_SEC_WOWLAN        21
#define IWL_UCODE_TLV_DEF_CALIB         22
#define IWL_UCODE_TLV_PHY_SKU           23
#define IWL_UCODE_TLV_API_CHANGES_SET   29
#define IWL_UCODE_TLV_ENABLED_CAPABILITIES 30
#define IWL_UCODE_TLV_N_SCAN_CHANNELS   31
#define IWL_UCODE_TLV_PAGING            32

#define IWL_UCODE_TLV_FLAGS_DW_BC_TABLE (1u << 4)

#define IWL_CPU1_CPU2_SEPARATOR_SECTION 0xFFFFCCCCu
#define IWL_PAGING_SEPARATOR_SECTION    0xAAAABBBBu

typedef struct {
    uint32_t zero;
    uint32_t magic;
    char     build[80];
} __attribute__((packed)) iwl_ucode_header_t;

_Static_assert(sizeof(iwl_ucode_header_t) == 88, "iwl_ucode_header_t must be 88 bytes");

typedef struct {
    uint32_t type;
    uint32_t length;
} __attribute__((packed)) iwl_ucode_tlv_t;

typedef struct {
    uint32_t ucode_type;
    uint32_t flow_trigger;
    uint32_t event_trigger;
} __attribute__((packed)) iwl_tlv_calib_ctrl_t;

/* Descriptor Ring Sizes */
#define IWL_TX_RING_SIZE                64
#define IWL_RX_RING_SIZE                64
#define IWL_TFD_QUEUE_SIZE_MAX          256
#define IWL_TFD_QUEUE_SIZE_BC_DUP       64
#define IWL_TFD_QUEUE_BC_SIZE           (IWL_TFD_QUEUE_SIZE_MAX + IWL_TFD_QUEUE_SIZE_BC_DUP)
#define IWL_MAX_QUEUES                  31
#define IWL_NUM_OF_TBS                  20

/* Hardware Transmit Frame Descriptor (TFD - 128 bytes) */
typedef struct {
    uint32_t lo;
    uint16_t hi_n_len;
} __attribute__((packed)) iwl_tfd_tb_t;

typedef struct {
    uint8_t      reserved1[3];
    uint8_t      num_tbs;
    iwl_tfd_tb_t tbs[IWL_NUM_OF_TBS];
    uint32_t     pad;
} __attribute__((packed)) iwl_tfd_t;

_Static_assert(sizeof(iwl_tfd_t) == 128, "iwl_tfd_t must be 128 bytes");

/* Scheduler Byte Count Table (640 bytes per queue) */
typedef struct {
    uint16_t tfd_offset[IWL_TFD_QUEUE_BC_SIZE];
} __attribute__((packed)) iwl_scd_bc_tbl_t;

_Static_assert(sizeof(iwl_scd_bc_tbl_t) == 640, "iwl_scd_bc_tbl_t must be 640 bytes");

/* Legacy Mock Tx / Rx Descriptors */
typedef struct {
    uint32_t buf_low;
    uint32_t buf_high;
    uint16_t len;
    uint16_t flags;
} __attribute__((packed)) iwl_tx_desc_t;

typedef struct {
    uint32_t buf_low;
    uint32_t buf_high;
} __attribute__((packed)) iwl_rx_desc_t;

/* MVM Host Command & Notification IDs */
#define IWL_HCMD_ALIVE                  0x01
#define IWL_INIT_COMPLETE_NOTIF         0x04
#define IWL_HCMD_PHY_CONTEXT            0x08
#define IWL_HCMD_ADD_STA                0x18
#define IWL_HCMD_REMOVE_STA             0x19
#define IWL_HCMD_TX                     0x1C
#define IWL_HCMD_SCD_QUEUE_CFG          0x1D
#define IWL_HCMD_MAC_CONTEXT            0x28
#define IWL_HCMD_TIME_EVENT             0x29
#define IWL_HCMD_BINDING_CONTEXT        0x2B
#define IWL_HCMD_TIME_QUOTA             0x2C
#define IWL_HCMD_SCAN_OFFLOAD_REQ       0x51
#define IWL_HCMD_PHY_CFG                0x6A
#define IWL_CALIB_RES_NOTIF_PHY_DB      0x6B
#define IWL_HCMD_PHY_DB                 0x6C
#define IWL_HCMD_POWER_TABLE            0x77
#define IWL_HCMD_SCAN_REQUEST           0x80
#define IWL_HCMD_NVM_ACCESS             0x88
#define IWL_HCMD_TX_ANT_CFG             0x98
#define IWL_HCMD_BT_CONFIG              0x9B
#define IWL_HCMD_MAC_PM_POWER_TABLE     0xA9
#define IWL_SCAN_ITER_COMPLETE_UMAC     0xB5
#define IWL_HCMD_RX_PHY                 0xC0
#define IWL_HCMD_RX_MPDU                0xC1
#define IWL_HCMD_MCC_UPDATE             0xC8
#define IWL_HCMD_SF_CFG                 0xD1
#define IWL_HCMD_BEACON_FILTERING       0xD2

/* Command Groups */
#define IWL_LONG_GROUP                  0x01
#define IWL_SYSTEM_GROUP                0x02
#define IWL_PHY_OPS_GROUP               0x04
#define IWL_DATA_PATH_GROUP             0x05

/* Wide Group Commands */
#define IWL_SCAN_CFG_CMD                0x0C /* in IWL_LONG_GROUP (0x01) */
#define IWL_SCAN_REQ_UMAC               0x0D /* in IWL_LONG_GROUP (0x01) */
#define IWL_SCAN_ABORT_UMAC             0x0E /* in IWL_LONG_GROUP (0x01) */
#define IWL_SCAN_COMPLETE_UMAC          0x0F /* in IWL_LONG_GROUP (0x01) */
#define IWL_FW_PAGING_BLOCK_CMD         0x4F /* in IWL_LONG_GROUP (0x01) */
#define IWL_TEMP_REPORT_THS_CMD         0x04 /* in IWL_PHY_OPS_GROUP (0x04) */
#define IWL_DQA_ENABLE_CMD              0x00 /* in IWL_DATA_PATH_GROUP (0x05) */

#define IWL_CMD_ID(opcode, group, ver)  ((uint32_t)(opcode) | ((uint32_t)(group) << 8) | ((uint32_t)(ver) << 16))
#define IWL_WIDE_ID(group, opcode)      (((uint16_t)(group) << 8) | (uint16_t)(opcode))

/* Command Headers */
typedef struct {
    uint8_t code;
    uint8_t flags;
    uint8_t idx;
    uint8_t qid;
} __attribute__((packed)) iwl_cmd_header_t;

typedef struct {
    uint8_t  opcode;
    uint8_t  group_id;
    uint8_t  idx;
    uint8_t  qid;
    uint16_t length;
    uint8_t  reserved;
    uint8_t  version;
} __attribute__((packed)) iwl_cmd_header_wide_t;

typedef struct {
    uint32_t         len_n_flags;
    iwl_cmd_header_t hdr;
    uint8_t          data[];
} __attribute__((packed)) iwl_rx_packet_t;

#define IWL_FH_RSCSR_FRAME_SIZE_MSK     0x00003FFFu
#define IWL_FH_RSCSR_FRAME_INVALID      0x55550000u
#define IWL_FH_RSCSR_FRAME_ALIGN        0x40u

typedef struct {
    uint8_t  cmd_id;
    uint8_t  subcmd_id;
    uint16_t sequence;
    uint8_t  flags;
    uint8_t  reserved[3];
} __attribute__((packed)) iwl_cmd_hdr_t;

/* Firmware Paging Command (0x014F) */
#define IWL_NUM_OF_FW_PAGING_BLOCKS     33
#define IWL_MAX_PAGING_DATA_BLOCKS      8   /* 8265 uses 6 data blocks + 1 CSS block */
#define IWL_PAGING_CMD_IS_SECURED       (1u << 9)
#define IWL_PAGING_CMD_IS_ENABLED       (1u << 8)

typedef struct {
    uint32_t flags;
    uint32_t block_size;
    uint32_t block_num;
    uint32_t device_phy_addr[IWL_NUM_OF_FW_PAGING_BLOCKS];
} __attribute__((packed)) iwl_fw_paging_cmd_t;

/* Smart FIFO Config Command (0xD1) */
#define IWL_SF_LONG_DELAY_ON            0
#define IWL_SF_FULL_ON                  1
#define IWL_SF_UNINIT                   2
#define IWL_SF_INIT_OFF                 3
#define IWL_SF_NUM_SCENARIO             5
#define IWL_SF_NUM_TIMEOUT_TYPES        2

typedef struct {
    uint32_t state;
    uint32_t watermark[2];
    uint32_t long_delay_timeouts[IWL_SF_NUM_SCENARIO][IWL_SF_NUM_TIMEOUT_TYPES];
    uint32_t full_on_timeouts[IWL_SF_NUM_SCENARIO][IWL_SF_NUM_TIMEOUT_TYPES];
} __attribute__((packed)) iwl_sf_cfg_cmd_t;

/* TX Antenna Config (0x98) */
typedef struct {
    uint32_t valid;
} __attribute__((packed)) iwl_tx_ant_cfg_cmd_t;

/* PHY Config Command (0x6A) */
typedef struct {
    uint32_t phy_cfg;
    uint32_t flow_trigger;
    uint32_t event_trigger;
} __attribute__((packed)) iwl_phy_cfg_cmd_t;

/* PHY DB Command / Notification (0x6C / 0x6B) */
typedef struct {
    uint16_t type;
    uint16_t length;
    uint8_t  data[];
} __attribute__((packed)) iwl_phy_db_cmd_t;

/* BT Coex Command (0x9B) */
typedef struct {
    uint32_t mode;
    uint32_t enabled_modules;
} __attribute__((packed)) iwl_bt_coex_cmd_t;

/* DQA Enable Command (0x0500) */
typedef struct {
    uint32_t cmd_queue;
} __attribute__((packed)) iwl_dqa_enable_cmd_t;

/* SCD Queue Config Command (0x1D) */
typedef struct {
    uint8_t  tid;
    uint8_t  scd_queue;
    uint8_t  enable;
    uint8_t  sta_id;
    uint8_t  tx_fifo;
    uint8_t  aggregate;
    uint16_t ssn;
    uint16_t window;
    uint16_t reserved;
} __attribute__((packed)) iwl_scd_txq_cfg_cmd_t;

_Static_assert(sizeof(iwl_scd_txq_cfg_cmd_t) == 12, "iwl_scd_txq_cfg_cmd_t must be 12 bytes");

/* Add Station Command v10 (0x18 - 68 bytes) */
#define IWL_STATION_ID                  0
#define IWL_AUX_STA_ID                  1
#define IWL_STA_LINK                    0
#define IWL_STA_AUX_ACTIVITY            4

typedef struct {
    uint8_t  add_modify;
    uint8_t  awake_acs;
    uint16_t tid_disable_tx;
    uint32_t mac_id_n_color;
    uint8_t  addr[6];
    uint16_t reserved2;
    uint8_t  sta_id;
    uint8_t  modify_mask;
    uint16_t reserved3;
    uint32_t station_flags;
    uint32_t station_flags_msk;
    uint8_t  add_immediate_ba_tid;
    uint8_t  remove_immediate_ba_tid;
    uint16_t add_immediate_ba_ssn;
    uint16_t sleep_tx_count;
    uint8_t  sleep_state_flags;
    uint8_t  station_type;
    uint16_t assoc_id;
    uint16_t beamform_flags;
    uint32_t tfd_queue_msk;
    uint16_t rx_ba_window;
    uint8_t  sp_length;
    uint8_t  uapsd_acs;
} __attribute__((packed)) iwl_add_sta_cmd_t;

_Static_assert(sizeof(iwl_add_sta_cmd_t) == 48, "iwl_add_sta_cmd_t must be 48 bytes");

/* PHY Context Command (0x08 - 40 bytes) */
#define IWL_FW_CTXT_ACTION_ADD          1
#define IWL_FW_CTXT_ACTION_MODIFY       2
#define IWL_FW_CTXT_ACTION_REMOVE       3
#define IWL_FW_CMD_ID_AND_COLOR(id, c)  (((uint32_t)(id) & 0xFFu) | (((uint32_t)(c) & 0xFFu) << 8))
#define IWL_PHY_BAND_5                  0
#define IWL_PHY_BAND_24                 1

typedef struct {
    uint32_t id_and_color;
    uint32_t action;
    uint32_t apply_time;
    uint32_t tx_param_color;
    struct {
        uint8_t band;
        uint8_t channel;
        uint8_t width;
        uint8_t ctrl_pos;
    } __attribute__((packed)) ci;
    uint32_t txchain_info;
    uint32_t rxchain_info;
    uint32_t acquisition_data;
    uint32_t dsp_cfg_flags;
} __attribute__((packed)) iwl_phy_context_cmd_t;

_Static_assert(sizeof(iwl_phy_context_cmd_t) == 36, "iwl_phy_context_cmd_t must be 36 bytes");

/* MAC Context Command (0x28) */
#define IWL_FW_MAC_TYPE_LISTENER        2
#define IWL_FW_MAC_TYPE_BSS_STA         5
#define IWL_MAC_INDEX_AUX               4
#define IWL_MAC_FILTER_IN_PROMISC       (1u << 0)
#define IWL_MAC_FILTER_IN_CONTROL_AND_MGMT (1u << 1)
#define IWL_MAC_FILTER_ACCEPT_GRP       (1u << 2)
#define IWL_MAC_FILTER_IN_BEACON        (1u << 6)
#define IWL_MAC_FILTER_IN_PROBE_REQUEST (1u << 12)

typedef struct {
    uint16_t cw_min;
    uint16_t cw_max;
    uint8_t  aifsn;
    uint8_t  fifos_mask;
    uint16_t edca_txop;
} __attribute__((packed)) iwl_ac_qos_t;

typedef struct {
    uint32_t is_assoc;
    uint32_t dtim_time;
    uint64_t dtim_tsf;
    uint32_t bi;
    uint32_t bi_reciprocal;
    uint32_t dtim_interval;
    uint32_t dtim_reciprocal;
    uint32_t listen_interval;
    uint32_t assoc_id;
    uint32_t assoc_beacon_arrive_time;
} __attribute__((packed)) iwl_mac_data_sta_t;

typedef struct {
    uint32_t     id_and_color;
    uint32_t     action;
    uint32_t     mac_type;
    uint32_t     tsf_id;
    uint8_t      node_addr[6];
    uint16_t     reserved_for_node_addr;
    uint8_t      bssid_addr[6];
    uint16_t     reserved_for_bssid_addr;
    uint32_t     cck_rates;
    uint32_t     ofdm_rates;
    uint32_t     protection_flags;
    uint32_t     cck_short_preamble;
    uint32_t     short_slot;
    uint32_t     filter_flags;
    uint32_t     qos_flags;
    iwl_ac_qos_t ac[5];
    union {
        iwl_mac_data_sta_t sta;
        uint8_t            raw[44];
    };
} __attribute__((packed)) iwl_mac_ctx_cmd_t;

/* Binding Context Command v1 (0x2B - 24 bytes) */
typedef struct {
    uint32_t id_and_color;
    uint32_t action;
    uint32_t macs[3];
    uint32_t phy;
} __attribute__((packed)) iwl_binding_cmd_v1_t;

_Static_assert(sizeof(iwl_binding_cmd_v1_t) == 24, "iwl_binding_cmd_v1_t must be 24 bytes");

/* Time Quota Command v2 (0x2C - 64 bytes) */
typedef struct {
    struct {
        uint32_t id_and_color;
        uint32_t quota;
        uint32_t max_duration;
        uint32_t low_latency;
    } __attribute__((packed)) quotas[4];
} __attribute__((packed)) iwl_time_quota_cmd_t;

/* Device Power Command (0x77) */
typedef struct {
    uint16_t flags;
    uint16_t reserved;
} __attribute__((packed)) iwl_device_power_cmd_t;

/* MCC Update Command v2 (0xC8 - 28 bytes) */
typedef struct {
    uint16_t mcc;
    uint8_t  source_id;
    uint8_t  reserved;
    uint32_t key;
    uint32_t reserved2[5];
} __attribute__((packed)) iwl_mcc_update_cmd_t;

_Static_assert(sizeof(iwl_mcc_update_cmd_t) == 28, "iwl_mcc_update_cmd_t must be 28 bytes");

/* UMAC Scan Config Command (0x010C) */
typedef struct {
    uint32_t flags;
    uint32_t tx_chains;
    uint32_t rx_chains;
    uint32_t legacy_rates;
    uint32_t out_of_channel_time;
    uint32_t suspend_time;
    uint8_t  dwell_active;
    uint8_t  dwell_passive;
    uint8_t  dwell_fragmented;
    uint8_t  dwell_extended;
    uint8_t  mac_addr[6];
    uint8_t  bcast_sta_id;
    uint8_t  channel_flags;
    uint8_t  channel_array[];
} __attribute__((packed)) iwl_scan_config_t;

_Static_assert(sizeof(iwl_scan_config_t) == 36, "iwl_scan_config_t must be 36 bytes");

/* UMAC Scan Request v7 (0x010D) */
typedef struct {
    uint32_t flags;
    uint8_t  channel_num;
    uint8_t  iter_count;
    uint16_t iter_interval;
} __attribute__((packed)) iwl_scan_channel_cfg_umac_t;

_Static_assert(sizeof(iwl_scan_channel_cfg_umac_t) == 8, "iwl_scan_channel_cfg_umac_t must be 8 bytes");

typedef struct {
    uint16_t offset;
    uint16_t len;
} __attribute__((packed)) iwl_scan_probe_segment_t;

typedef struct {
    iwl_scan_probe_segment_t mac_header;
    iwl_scan_probe_segment_t band_data[2];
    iwl_scan_probe_segment_t common_data;
    uint8_t                  buf[512];
} __attribute__((packed)) iwl_scan_probe_req_v1_t;

typedef struct {
    uint8_t id;
    uint8_t len;
    uint8_t ssid[32];
} __attribute__((packed)) iwl_ssid_ie_t;

typedef struct {
    struct {
        uint16_t interval;
        uint8_t  iter_count;
        uint8_t  reserved;
    } __attribute__((packed)) schedule[2];
    uint16_t                delay;
    uint16_t                reserved;
    iwl_scan_probe_req_v1_t preq;
    iwl_ssid_ie_t           direct_scan[20];
} __attribute__((packed)) iwl_scan_req_umac_tail_v1_t;

typedef struct {
    uint32_t flags;
    uint32_t uid;
    uint32_t ooc_priority;
    uint16_t general_flags;
    uint8_t  reserved;
    uint8_t  scan_start_mac_id;
    uint8_t  active_dwell;
    uint8_t  passive_dwell;
    uint8_t  fragmented_dwell;
    uint8_t  adwell_default_n_aps;
    uint8_t  adwell_default_n_aps_social;
    uint8_t  reserved3;
    uint16_t adwell_max_budget;
    uint32_t max_out_time[2];
    uint32_t suspend_time[2];
    uint32_t scan_priority;
    struct {
        uint8_t  flags;
        uint8_t  count;
        uint16_t reserved;
    } __attribute__((packed)) channel;
} __attribute__((packed)) iwl_scan_req_umac_v7_t;

_Static_assert(sizeof(iwl_scan_req_umac_v7_t) == 48, "iwl_scan_req_umac_v7_t must be 48 bytes");

/* RX PHY Info (0xC0) & MPDU (0xC1) */
typedef struct {
    uint8_t  non_cfg_phy_cnt;
    uint8_t  cfg_phy_cnt;
    uint8_t  stat_id;
    uint8_t  reserved1;
    uint32_t system_timestamp;
    uint64_t timestamp;
    uint32_t beacon_time_stamp;
    uint16_t phy_flags;
    uint16_t channel;
    uint32_t non_cfg_phy[8];
    uint32_t rate_n_flags;
    uint32_t byte_count;
    uint16_t mac_active_msk;
    uint16_t frame_time;
} __attribute__((packed)) iwl_rx_phy_info_t;

/* pbuf->flags metadata encoding for RSSI and Channel from 0xC0 */
#define IWL_PBUF_META_VALID             (1u << 23)
#define IWL_PBUF_PACK_META(rssi, chan)  (IWL_PBUF_META_VALID | (((uint32_t)(uint8_t)(rssi)) << 24) | (((uint32_t)((chan) & 0x7Fu)) << 16))
#define IWL_PBUF_GET_RSSI(flags)        ((int8_t)((flags) >> 24))
#define IWL_PBUF_GET_CHAN(flags)        ((uint8_t)(((flags) >> 16) & 0x7Fu))

/* Device Driver State */
typedef struct {
    bool         detected;
    bool         initialized;
    bool         firmware_alive;
    bool         mvm_initialized;
    pci_device_t pci;
    uintptr_t    mmio_base;
    uint64_t     mmio_size;
    uint8_t      mac_addr[6];
    uint8_t      current_channel;

    /* DMA Rings */
    uint32_t     tx_head;
    uint32_t     tx_tail;
    uint32_t     rx_head;
    uint32_t     rx_tail;

    spinlock_t   lock;
} iwlwifi_state_t;

/* Virtual MMIO mapping base */
#define IWLWIFI_MMIO_VIRT               0xFFFFFFFFE3000000ULL

/* Public API */
bool iwlwifi_probe(void);
bool iwlwifi_map_mmio(void);
bool iwlwifi_is_supported_device(uint16_t vendor, uint16_t device);
bool iwlwifi_init_hw(void);
bool iwlwifi_load_firmware(const void *fw_data, size_t fw_len);
int  iwlwifi_start_scan(const char *ssid);
int  iwlwifi_send_raw(const void *buf, size_t len);
pbuf_t *iwlwifi_poll_raw(void);
void iwlwifi_recycle_raw(pbuf_t *p);
int  iwlwifi_set_channel(uint8_t channel);
int  iwlwifi_get_mac(uint8_t mac[6]);
bool iwlwifi_is_detected(void);

#endif /* FORTRESS_IWLWIFI_H */
