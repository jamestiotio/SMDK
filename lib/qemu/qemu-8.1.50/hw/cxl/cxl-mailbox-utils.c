/*
 * CXL Utility library for mailbox interface
 *
 * Copyright(C) 2020 Intel Corporation.
 *
 * This work is licensed under the terms of the GNU GPL, version 2. See the
 * COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "hw/cxl/cxl.h"
#include "hw/cxl/cxl_events.h"
#include "hw/cxl/cxl_mailbox.h"
#include "hw/pci/pci.h"
#include "hw/pci-bridge/cxl_upstream_port.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qemu/uuid.h"
#include "sysemu/hostmem.h"

#define CXL_CAPACITY_MULTIPLIER   (256 * MiB)
#define CXL_DC_EVENT_LOG_SIZE 8

/*
 * How to add a new command, example. The command set FOO, with cmd BAR.
 *  1. Add the command set and cmd to the enum.
 *     FOO    = 0x7f,
 *          #define BAR 0
 *  2. Implement the handler
 *    static CXLRetCode cmd_foo_bar(struct cxl_cmd *cmd,
 *                                  CXLDeviceState *cxl_dstate, uint16_t *len)
 *  3. Add the command to the cxl_cmd_set[][]
 *    [FOO][BAR] = { "FOO_BAR", cmd_foo_bar, x, y },
 *  4. Implement your handler
 *     define_mailbox_handler(FOO_BAR) { ... return CXL_MBOX_SUCCESS; }
 *
 *
 *  Writing the handler:
 *    The handler will provide the &struct cxl_cmd, the &CXLDeviceState, and the
 *    in/out length of the payload. The handler is responsible for consuming the
 *    payload from cmd->payload and operating upon it as necessary. It must then
 *    fill the output data into cmd->payload (overwriting what was there),
 *    setting the length, and returning a valid return code.
 *
 *  XXX: The handler need not worry about endianess. The payload is read out of
 *  a register interface that already deals with it.
 */

enum {
    INFOSTAT    = 0x00,
        #define IS_IDENTIFY   0x1
        #define BACKGROUND_OPERATION_STATUS    0x2
    EVENTS      = 0x01,
        #define GET_RECORDS   0x0
        #define CLEAR_RECORDS   0x1
        #define GET_INTERRUPT_POLICY   0x2
        #define SET_INTERRUPT_POLICY   0x3
    FIRMWARE_UPDATE = 0x02,
        #define GET_INFO      0x0
    TIMESTAMP   = 0x03,
        #define GET           0x0
        #define SET           0x1
    LOGS        = 0x04,
        #define GET_SUPPORTED 0x0
        #define GET_LOG       0x1
    IDENTIFY    = 0x40,
        #define MEMORY_DEVICE 0x0
    CCLS        = 0x41,
        #define GET_PARTITION_INFO     0x0
        #define GET_LSA       0x2
        #define SET_LSA       0x3
    SANITIZE    = 0x44,
        #define OVERWRITE     0x0
        #define SECURE_ERASE  0x1
    PERSISTENT_MEM = 0x45,
        #define GET_SECURITY_STATE     0x0
    MEDIA_AND_POISON = 0x43,
        #define GET_POISON_LIST        0x0
        #define INJECT_POISON          0x1
        #define CLEAR_POISON           0x2
        #define GET_SCAN_MEDIA_CAPABILITIES 0x3
        #define SCAN_MEDIA             0x4
        #define GET_SCAN_MEDIA_RESULTS 0x5
    DCD_CONFIG  = 0x48,
        #define GET_DC_CONFIG          0x0
        #define GET_DYN_CAP_EXT_LIST   0x1
        #define ADD_DYN_CAP_RSP        0x2
        #define RELEASE_DYN_CAP        0x3
    PHYSICAL_SWITCH = 0x51,
        #define IDENTIFY_SWITCH_DEVICE      0x0
        #define GET_PHYSICAL_PORT_STATE     0x1
    TUNNEL = 0x53,
        #define MANAGEMENT_COMMAND     0x0
    MHD = 0x55,
        #define GET_MHD_INFO 0x0
};

/* CCI Message Format CXL r3.0 Figure 7-19 */
typedef struct CXLCCIMessage {
    uint8_t category;
    uint8_t tag;
    uint8_t resv1;
    uint8_t command;
    uint8_t command_set;
    uint8_t pl_length[3];
    uint16_t vendor_specific;
    uint16_t rc;
    uint8_t payload[];
} QEMU_PACKED CXLCCIMessage;

static CXLRetCode cmd_tunnel_management_cmd(const struct cxl_cmd *cmd,
                                            uint8_t *payload_in,
                                            size_t len_in,
                                            uint8_t *payload_out,
                                            size_t *len_out,
                                            CXLCCI *cci)
{
    CXLUpstreamPort *usp = CXL_USP(cci->d);
    PCIDevice *tunnel_target;
    struct {
        uint8_t port_or_ld_id;
        uint8_t target_type;
        uint16_t size;
        CXLCCIMessage ccimessage;
    } *in;
    struct {
        uint16_t resp_len;
        uint8_t resv[2];
        CXLCCIMessage ccimessage;
    } *out;

    if (cmd->in < sizeof(*in)) {
        return CXL_MBOX_INVALID_INPUT;
    }
    in = (void *)payload_in;
    out = (void *)payload_out;

    if (cmd->in < sizeof(*in) + in->size) {
        return CXL_MBOX_INVALID_INPUT;
    }
    if (in->size < 3 * sizeof(uint32_t)) {
        return CXL_MBOX_INVALID_INPUT;
    }
    /* Need to find target CCI */
    //Lets assume simple tunnel to port - find that device.
    if (in->target_type != 0) {
        printf("QEMU: sent to FM-LD which makes no sense yet\n");
    }

    tunnel_target = pcie_find_port_by_pn(&PCI_BRIDGE(usp)->sec_bus,
                                         in->port_or_ld_id);
    if (!tunnel_target) {
        return CXL_MBOX_INVALID_INPUT;
    }

    tunnel_target =
        pci_bridge_get_sec_bus(PCI_BRIDGE(tunnel_target))->devices[0];
    if (!tunnel_target) {
        return CXL_MBOX_INVALID_INPUT;
    }

    if (object_dynamic_cast(OBJECT(tunnel_target), TYPE_CXL_TYPE3)) {
        CXLType3Dev *ct3d = CXL_TYPE3(tunnel_target);
        size_t pl_length = in->ccimessage.pl_length[2] << 16 |
            in->ccimessage.pl_length[1] << 8 | in->ccimessage.pl_length[0];
        size_t length_out;
        bool bg_started;
        int rc;

        rc = cxl_process_cci_message(&ct3d->vdm_mctp_cci,
                                     in->ccimessage.command_set,
                                     in->ccimessage.command,
                                     pl_length, in->ccimessage.payload,
                                     &length_out, out->ccimessage.payload,
                                     &bg_started);
        /* Payload should be in place. Rest of CCI header and needs filling */
        out->resp_len = length_out + sizeof(CXLCCIMessage); /* CHECK */
        st24_le_p(out->ccimessage.pl_length, length_out);
        out->ccimessage.rc = rc;
        printf("len_out is %lu\n", length_out);
        *len_out = length_out + sizeof(*out);

        return CXL_MBOX_SUCCESS;
    }

    return CXL_MBOX_INVALID_INPUT;
}

/*
 * CXL r3.0 section 7.6.7.5.1 - Get Multi-Headed Info (Opcode 5500h)
 */
static CXLRetCode cmd_mhd_get_info(const struct cxl_cmd *cmd,
                                   uint8_t *payload_in, size_t len_in,
                                   uint8_t *payload_out, size_t *len_out,
                                   CXLCCI *cci)
{
    CXLType3Dev *ct3d = CXL_TYPE3(cci->d);
    CXLType3Class *cvc = CXL_TYPE3_GET_CLASS(ct3d);
    if (cvc->mhd_get_info) {
        return cvc->mhd_get_info(cmd, payload_in, len_in, payload_out,
                                 len_out, cci);
    }
    return CXL_MBOX_UNSUPPORTED;
}

static CXLRetCode cmd_events_get_records(const struct cxl_cmd *cmd,
                                         uint8_t *payload_in, size_t len_in,
                                         uint8_t *payload_out, size_t *len_out,
                                         CXLCCI *cci)
{
    CXLDeviceState *cxlds = &CXL_TYPE3(cci->d)->cxl_dstate;
    CXLGetEventPayload *pl;
    uint8_t log_type;
    int max_recs;

    if (cmd->in < sizeof(log_type)) {
        return CXL_MBOX_INVALID_INPUT;
    }

    log_type = payload_in[0];

    pl = (CXLGetEventPayload *)payload_out;
    memset(pl, 0, sizeof(*pl));

    max_recs = (cxlds->payload_size - CXL_EVENT_PAYLOAD_HDR_SIZE) /
                CXL_EVENT_RECORD_SIZE;
    if (max_recs > 0xFFFF) {
        max_recs = 0xFFFF;
    }

    return cxl_event_get_records(cxlds, pl, log_type, max_recs, len_out);
}

static CXLRetCode cmd_events_clear_records(const struct cxl_cmd *cmd,
                                           uint8_t *payload_in,
                                           size_t len_in,
                                           uint8_t *payload_out,
                                           size_t *len_out,
                                           CXLCCI *cci)
{
    CXLDeviceState *cxlds = &CXL_TYPE3(cci->d)->cxl_dstate;
    CXLClearEventPayload *pl;

    pl = (CXLClearEventPayload *)payload_in;
    *len_out = 0;
    return cxl_event_clear_records(cxlds, pl);
}

static CXLRetCode cmd_events_get_interrupt_policy(const struct cxl_cmd *cmd,
                                                  uint8_t *payload_in,
                                                  size_t len_in,
                                                  uint8_t *payload_out,
                                                  size_t *len_out,
                                                  CXLCCI *cci)
{
    CXLDeviceState *cxlds = &CXL_TYPE3(cci->d)->cxl_dstate;
    CXLEventInterruptPolicy *policy;
    CXLEventLog *log;

    policy = (CXLEventInterruptPolicy *)payload_out;
    memset(policy, 0, sizeof(*policy));

    log = &cxlds->event_logs[CXL_EVENT_TYPE_INFO];
    if (log->irq_enabled) {
        policy->info_settings = CXL_EVENT_INT_SETTING(log->irq_vec);
    }

    log = &cxlds->event_logs[CXL_EVENT_TYPE_WARN];
    if (log->irq_enabled) {
        policy->warn_settings = CXL_EVENT_INT_SETTING(log->irq_vec);
    }

    log = &cxlds->event_logs[CXL_EVENT_TYPE_FAIL];
    if (log->irq_enabled) {
        policy->failure_settings = CXL_EVENT_INT_SETTING(log->irq_vec);
    }

    log = &cxlds->event_logs[CXL_EVENT_TYPE_FATAL];
    if (log->irq_enabled) {
        policy->fatal_settings = CXL_EVENT_INT_SETTING(log->irq_vec);
    }

    log = &cxlds->event_logs[CXL_EVENT_TYPE_DYNAMIC_CAP];
    if (log->irq_enabled) {
        /* Dynamic Capacity borrows the same vector as info */
        policy->dyn_cap_settings = CXL_INT_MSI_MSIX;
    }

    *len_out = sizeof(*policy);
    return CXL_MBOX_SUCCESS;
}

static CXLRetCode cmd_events_set_interrupt_policy(const struct cxl_cmd *cmd,
                                                  uint8_t *payload_in,
                                                  size_t len_in,
                                                  uint8_t *payload_out,
                                                  size_t *len_out,
                                                  CXLCCI *cci)
{
    CXLDeviceState *cxlds = &CXL_TYPE3(cci->d)->cxl_dstate;
    CXLEventInterruptPolicy *policy;
    CXLEventLog *log;

    if (len_in < CXL_EVENT_INT_SETTING_MIN_LEN) {
        return CXL_MBOX_INVALID_PAYLOAD_LENGTH;
    }

    policy = (CXLEventInterruptPolicy *)payload_in;

    log = &cxlds->event_logs[CXL_EVENT_TYPE_INFO];
    log->irq_enabled = (policy->info_settings & CXL_EVENT_INT_MODE_MASK) ==
                        CXL_INT_MSI_MSIX;

    log = &cxlds->event_logs[CXL_EVENT_TYPE_WARN];
    log->irq_enabled = (policy->warn_settings & CXL_EVENT_INT_MODE_MASK) ==
                        CXL_INT_MSI_MSIX;

    log = &cxlds->event_logs[CXL_EVENT_TYPE_FAIL];
    log->irq_enabled = (policy->failure_settings & CXL_EVENT_INT_MODE_MASK) ==
                        CXL_INT_MSI_MSIX;

    log = &cxlds->event_logs[CXL_EVENT_TYPE_FATAL];
    log->irq_enabled = (policy->fatal_settings & CXL_EVENT_INT_MODE_MASK) ==
                        CXL_INT_MSI_MSIX;

    /* DCD is optional */
    if (len_in < sizeof(*policy)) {
        return CXL_MBOX_SUCCESS;
    }

    log = &cxlds->event_logs[CXL_EVENT_TYPE_DYNAMIC_CAP];
    log->irq_enabled = (policy->dyn_cap_settings & CXL_EVENT_INT_MODE_MASK) ==
                        CXL_INT_MSI_MSIX;

    *len_out = 0;
    return CXL_MBOX_SUCCESS;
}

/* CXL r3 8.2.9.1.1 */
static CXLRetCode cmd_infostat_identify(const struct cxl_cmd *cmd,
                                        uint8_t *payload_in,
                                        size_t len_in,
                                        uint8_t *payload_out,
                                        size_t *len_out,
                                        CXLCCI *cci)
{
    PCIDeviceClass *class = PCI_DEVICE_GET_CLASS(cci->d);
    struct {
        uint16_t pcie_vid;
        uint16_t pcie_did;
        uint16_t pcie_subsys_vid;
        uint16_t pcie_subsys_id;
        uint64_t sn;
    uint8_t max_message_size;
        uint8_t component_type;
    } QEMU_PACKED *is_identify;
    QEMU_BUILD_BUG_ON(sizeof(*is_identify) != 18);

    is_identify = (void *)payload_out;
    memset(is_identify, 0, sizeof(*is_identify));
    /*
     * Messy question - which IDs?  Those of the CCI Function, or those of
     * the USP?
     */
    is_identify->pcie_vid = class->vendor_id;
    is_identify->pcie_did = class->device_id;
    if (object_dynamic_cast(OBJECT(cci->d), TYPE_CXL_USP)) {
        is_identify->sn = CXL_USP(cci->d)->sn;
        /* Subsystem info not defined for a USP */
        is_identify->pcie_subsys_vid = 0;
        is_identify->pcie_subsys_id = 0;
        is_identify->component_type = 0x0; /* Switch */
    } else if (object_dynamic_cast(OBJECT(cci->d), TYPE_CXL_TYPE3)) {
        is_identify->sn = CXL_TYPE3(cci->d)->sn;
        is_identify->pcie_subsys_vid = class->subsystem_vendor_id;
        is_identify->pcie_subsys_id = class->subsystem_id;
        is_identify->component_type = 0x3; /* Type 3 */
    }

    /* FIXME: This depends on interface */
    is_identify->max_message_size = CXL_MAILBOX_PAYLOAD_SHIFT;
    *len_out = sizeof(*is_identify);
    return CXL_MBOX_SUCCESS;
}

static void cxl_set_dsp_active_bm(PCIBus *b, PCIDevice *d,
                                  void *private)
{
    uint8_t *bm = private;
    if (object_dynamic_cast(OBJECT(d), TYPE_CXL_DSP)) {
        uint8_t port = PCIE_PORT(d)->port;
        bm[port / 8] |= 1 << (port % 8);
    }
}

/* CXL r3 8.2.9.1.1 */
static CXLRetCode cmd_identify_switch_device(const struct cxl_cmd *cmd,
                                             uint8_t *payload_in,
                                             size_t len_in,
                                             uint8_t *payload_out,
                                             size_t *len_out,
                                             CXLCCI *cci)
{
    PCIEPort *usp = PCIE_PORT(cci->d);
    PCIBus *bus = &PCI_BRIDGE(cci->d)->sec_bus;
    int num_phys_ports = pcie_count_ds_ports(bus);

    struct cxl_fmapi_ident_switch_dev_resp_pl {
        uint8_t ingress_port_id;
        uint8_t rsvd;
        uint8_t num_physical_ports;
        uint8_t num_vcss;
        uint8_t active_port_bitmask[0x20];
        uint8_t active_vcs_bitmask[0x20];
        uint16_t total_vppbs;
        uint16_t bound_vppbs;
        uint8_t num_hdm_decoders_per_usp;
    } QEMU_PACKED *out;
    QEMU_BUILD_BUG_ON(sizeof(*out) != 0x49);

    out = (struct cxl_fmapi_ident_switch_dev_resp_pl *)payload_out;
    *out = (struct cxl_fmapi_ident_switch_dev_resp_pl) {
        .num_physical_ports = num_phys_ports + 1, /* 1 USP */
        .num_vcss = 1, /* Not yet support multiple VCS - potentialy tricky */
        .active_vcs_bitmask[0] = 0x1,
        .total_vppbs = num_phys_ports + 1,
        .bound_vppbs = num_phys_ports + 1,
        .num_hdm_decoders_per_usp = 4,
    };

    /* Depends on the CCI type */
    if (object_dynamic_cast(OBJECT(cci->intf), TYPE_PCIE_PORT)) {
        out->ingress_port_id = PCIE_PORT(cci->intf)->port;
    } else {
        /* MCTP? */
        out->ingress_port_id = 0;
    }

    pci_for_each_device_under_bus(bus, cxl_set_dsp_active_bm,
                                  out->active_port_bitmask);
    out->active_port_bitmask[usp->port / 8] |= (1 << usp->port % 8);

    *len_out = sizeof(*out);

    return CXL_MBOX_SUCCESS;
}

static CXLRetCode cmd_get_physical_port_state(const struct cxl_cmd *cmd,
                                              uint8_t *payload_in,
                                              size_t len_in,
                                              uint8_t *payload_out,
                                              size_t *len_out,
                                              CXLCCI *cci)
{
    /*
     * CXL r3.0 7.6.7.1.2 Get Physical Port State (Opcode 5101h)
     */
    /* CXL r3.0 Table 7-18 Get Physical Port State Request Payload */
    struct cxl_fmapi_get_phys_port_state_req_pl {
        uint8_t num_ports; /* CHECK. may get too large for MCTP message size */
        uint8_t ports[];
    } QEMU_PACKED *in;

    /*
     * CXL r3.0 Table 7-20 Get Physical Port State Port Information Block
     * Format
     */
    struct cxl_fmapi_port_state_info_block {
        uint8_t port_id;
        uint8_t config_state;
        uint8_t connected_device_cxl_version;
        uint8_t rsv1;
        uint8_t connected_device_type;
        uint8_t port_cxl_version_bitmask;
        uint8_t max_link_width;
        uint8_t negotiated_link_width;
        uint8_t supported_link_speeds_vector;
        uint8_t max_link_speed;
        uint8_t current_link_speed;
        uint8_t ltssm_state;
        uint8_t first_lane_num;
        uint16_t link_state;
        uint8_t supported_ld_count;
    } QEMU_PACKED;

    /* CXL r3.0 Table 7-19 Get Physical Port State Response Payload */
    struct cxl_fmapi_get_phys_port_state_resp_pl {
        uint8_t num_ports;
        uint8_t rsv1[3];
        struct cxl_fmapi_port_state_info_block ports[];
    } QEMU_PACKED *out;
    PCIBus *bus = &PCI_BRIDGE(cci->d)->sec_bus;
    int num_phys_ports = pcie_count_ds_ports(bus);
    int i;
    size_t pl_size;

    in = (struct cxl_fmapi_get_phys_port_state_req_pl *)payload_in;
    out = (struct cxl_fmapi_get_phys_port_state_resp_pl *)payload_out;
    /* Not currently matching against requested  */
    out->num_ports = num_phys_ports;

    for (i = 0; i < out->num_ports; i++) {
        struct cxl_fmapi_port_state_info_block *port;
        port = &out->ports[i];
        port->port_id = i; /* TODO: Right port number */
        if (port->port_id < 1) { /* 1 upstream ports */
            port->config_state = 4;
            port->connected_device_type = 0;
        } else { /* remainder downstream ports */
            port->config_state = 3;
            port->connected_device_type = 4; /* TODO: Check. CXL type 3 */
            port->supported_ld_count = 3;
        }
        port->connected_device_cxl_version = 2;
        port->port_cxl_version_bitmask = 0x2;
        port->max_link_width = 0x10; /* x16 */
        port->negotiated_link_width = 0x10;
        port->supported_link_speeds_vector = 0x1c; /* 8, 16, 32 GT/s */
        port->max_link_speed = 5;
        port->current_link_speed = 5; /* 32 */
        port->ltssm_state = 0x7; /* L2 */
        port->first_lane_num = 0;
        port->link_state = 0;
    }

    pl_size = sizeof(out) + sizeof(*out->ports) * in->num_ports;

    *len_out = pl_size;

    return CXL_MBOX_SUCCESS;
}

/* CXL r3.0 8.2.9.1.2 */
static CXLRetCode cmd_infostat_bg_op_sts(const struct cxl_cmd *cmd,
                                         uint8_t *payload_in,
                                         size_t len_in,
                                         uint8_t *payload_out,
                                         size_t *len_out,
                                         CXLCCI *cci)
{
    struct {
        uint8_t status;
        uint8_t rsvd;
        uint16_t opcode;
        uint16_t returncode;
        uint16_t vendor_ext_status;
    } QEMU_PACKED *bg_op_status;
    QEMU_BUILD_BUG_ON(sizeof(*bg_op_status) != 8);

    bg_op_status = (void *)payload_out;
    memset(bg_op_status, 0, sizeof(*bg_op_status));
    bg_op_status->status = cci->bg.complete_pct << 1;
    if (cci->bg.runtime > 0) {
        bg_op_status->status |= 1U << 0;
    }
    bg_op_status->opcode = cci->bg.opcode;
    bg_op_status->returncode = cci->bg.ret_code;
    *len_out = sizeof(*bg_op_status);
    return CXL_MBOX_SUCCESS;
}

/* 8.2.9.2.1 */
static CXLRetCode cmd_firmware_update_get_info(const struct cxl_cmd *cmd,
                                               uint8_t *payload_in,
                                               size_t len,
                                               uint8_t *payload_out,
                                               size_t *len_out,
                                               CXLCCI *cci)
{
    CXLType3Dev *ct3d = CXL_TYPE3(cci->d);
    CXLDeviceState *cxl_dstate = &ct3d->cxl_dstate;
    struct {
        uint8_t slots_supported;
        uint8_t slot_info;
        uint8_t caps;
        uint8_t rsvd[0xd];
        char fw_rev1[0x10];
        char fw_rev2[0x10];
        char fw_rev3[0x10];
        char fw_rev4[0x10];
    } QEMU_PACKED *fw_info;
    QEMU_BUILD_BUG_ON(sizeof(*fw_info) != 0x50);

    if ((cxl_dstate->vmem_size < CXL_CAPACITY_MULTIPLIER) ||
        (cxl_dstate->pmem_size < CXL_CAPACITY_MULTIPLIER) ||
        (ct3d->dc.total_capacity < CXL_CAPACITY_MULTIPLIER)) {
        return CXL_MBOX_INTERNAL_ERROR;
    }

    fw_info = (void *)payload_out;
    memset(fw_info, 0, sizeof(*fw_info));

    fw_info->slots_supported = 2;
    fw_info->slot_info = BIT(0) | BIT(3);
    fw_info->caps = 0;
    pstrcpy(fw_info->fw_rev1, sizeof(fw_info->fw_rev1), "BWFW VERSION 0");

    *len_out = sizeof(*fw_info);
    return CXL_MBOX_SUCCESS;
}

/* 8.2.9.3.1 */
static CXLRetCode cmd_timestamp_get(const struct cxl_cmd *cmd,
                                    uint8_t *payload_in,
                                    size_t len_in,
                                    uint8_t *payload_out,
                                    size_t *len_out,
                                    CXLCCI *cci)
{
    CXLDeviceState *cxl_dstate = &CXL_TYPE3(cci->d)->cxl_dstate;
    uint64_t final_time = cxl_device_get_timestamp(cxl_dstate);

    stq_le_p(payload_out, final_time);
    *len_out = 8;

    return CXL_MBOX_SUCCESS;
}

/* 8.2.9.3.2 */
static CXLRetCode cmd_timestamp_set(const struct cxl_cmd *cmd,
                                    uint8_t *payload_in,
                                    size_t len_in,
                                    uint8_t *payload_out,
                                    size_t *len_out,
                                    CXLCCI *cci)
{
    CXLDeviceState *cxl_dstate = &CXL_TYPE3(cci->d)->cxl_dstate;

    cxl_dstate->timestamp.set = true;
    cxl_dstate->timestamp.last_set = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    cxl_dstate->timestamp.host_set = le64_to_cpu(*(uint64_t *)payload_in);

    *len_out = 0;
    return CXL_MBOX_SUCCESS;
}

/* CXL r3.0 seciton 8.2.9.5.2.1: Command Effects Log (CEL) */
static const QemuUUID cel_uuid = {
    .data = UUID(0x0da9c0b5, 0xbf41, 0x4b78, 0x8f, 0x79,
                 0x96, 0xb1, 0x62, 0x3b, 0x3f, 0x17)
};

/* CXL r3.0 section 8.2.9.5.1: Get Supported Log (Opcode 0400h) */
static CXLRetCode cmd_logs_get_supported(const struct cxl_cmd *cmd,
                                         uint8_t *payload_in,
                                         size_t len_in,
                                         uint8_t *payload_out,
                                         size_t *len_out,
                                         CXLCCI *cci)
{
    struct {
        uint16_t entries;
        uint8_t rsvd[6];
        struct {
            QemuUUID uuid;
            uint32_t size;
        } log_entries[1];
    } QEMU_PACKED *supported_logs = (void *)payload_out;
    QEMU_BUILD_BUG_ON(sizeof(*supported_logs) != 0x1c);

    supported_logs->entries = 1;
    supported_logs->log_entries[0].uuid = cel_uuid;
    supported_logs->log_entries[0].size = 4 * cci->cel_size;

    *len_out = sizeof(*supported_logs);
    return CXL_MBOX_SUCCESS;
}

/* CXL r3.0 section 8.2.9.5.2: Get Log (Opcode 0x401h) */
static CXLRetCode cmd_logs_get_log(const struct cxl_cmd *cmd,
                                   uint8_t *payload_in,
                                   size_t len_in,
                                   uint8_t *payload_out,
                                   size_t *len_out,
                                   CXLCCI *cci)
{
    struct {
        QemuUUID uuid;
        uint32_t offset;
        uint32_t length;
    } QEMU_PACKED QEMU_ALIGNED(16) *get_log;

    get_log = (void *)payload_in;

    /*
     * XXX: Spec doesn't address incorrect UUID incorrectness.
     *
     * The CEL buffer is large enough to fit all commands in the emulation, so
     * the only possible failure would be if the mailbox itself isn't big
     * enough.
     */
    if (get_log->offset + get_log->length > cci->payload_max) {
        return CXL_MBOX_INVALID_INPUT;
    }

    if (!qemu_uuid_is_equal(&get_log->uuid, &cel_uuid)) {
        return CXL_MBOX_UNSUPPORTED;
    }

    /* Store off everything to local variables so we can wipe out the payload */
    *len_out = get_log->length;

    memmove(payload_out, cci->cel_log + get_log->offset, get_log->length);

    return CXL_MBOX_SUCCESS;
}

/* 8.2.9.5.1.1 */
static CXLRetCode cmd_identify_memory_device(const struct cxl_cmd *cmd,
                                             uint8_t *payload_in,
                                             size_t len_in,
                                             uint8_t *payload_out,
                                             size_t *len_out,
                                             CXLCCI *cci)
{
    struct {
        char fw_revision[0x10];
        uint64_t total_capacity;
        uint64_t volatile_capacity;
        uint64_t persistent_capacity;
        uint64_t partition_align;
        uint16_t info_event_log_size;
        uint16_t warning_event_log_size;
        uint16_t failure_event_log_size;
        uint16_t fatal_event_log_size;
        uint32_t lsa_size;
        uint8_t poison_list_max_mer[3];
        uint16_t inject_poison_limit;
        uint8_t poison_caps;
        uint8_t qos_telemetry_caps;
        uint16_t dc_event_log_size;
    } QEMU_PACKED *id;
    QEMU_BUILD_BUG_ON(sizeof(*id) != 0x45);
    CXLType3Dev *ct3d = CXL_TYPE3(cci->d);
    CXLType3Class *cvc = CXL_TYPE3_GET_CLASS(ct3d);
    CXLDeviceState *cxl_dstate = &ct3d->cxl_dstate;

    if ((!QEMU_IS_ALIGNED(cxl_dstate->vmem_size, CXL_CAPACITY_MULTIPLIER)) ||
        (!QEMU_IS_ALIGNED(cxl_dstate->pmem_size, CXL_CAPACITY_MULTIPLIER)) ||
        (!QEMU_IS_ALIGNED(ct3d->dc.total_capacity, CXL_CAPACITY_MULTIPLIER))) {
        return CXL_MBOX_INTERNAL_ERROR;
    }

    id = (void *)payload_out;
    memset(id, 0, sizeof(*id));

    snprintf(id->fw_revision, 0x10, "BWFW VERSION %02d", 0);

    stq_le_p(&id->total_capacity,
            cxl_dstate->static_mem_size / CXL_CAPACITY_MULTIPLIER);
    stq_le_p(&id->persistent_capacity,
             cxl_dstate->pmem_size / CXL_CAPACITY_MULTIPLIER);
    stq_le_p(&id->volatile_capacity,
             cxl_dstate->vmem_size / CXL_CAPACITY_MULTIPLIER);
    stl_le_p(&id->lsa_size, cvc->get_lsa_size(ct3d));
    /* 256 poison records */
    st24_le_p(id->poison_list_max_mer, 256);
    /* No limit - so limited by main poison record limit */
    stw_le_p(&id->inject_poison_limit, 0);
    stw_le_p(&id->dc_event_log_size, CXL_DC_EVENT_LOG_SIZE);

    *len_out = sizeof(*id);
    return CXL_MBOX_SUCCESS;
}

static CXLRetCode cmd_ccls_get_partition_info(const struct cxl_cmd *cmd,
                                              uint8_t *payload_in,
                                              size_t len_in,
                                              uint8_t *payload_out,
                                              size_t *len_out,
                                              CXLCCI *cci)
{
    CXLDeviceState *cxl_dstate = &CXL_TYPE3(cci->d)->cxl_dstate;
    struct {
        uint64_t active_vmem;
        uint64_t active_pmem;
        uint64_t next_vmem;
        uint64_t next_pmem;
    } QEMU_PACKED *part_info = (void *)payload_out;
    QEMU_BUILD_BUG_ON(sizeof(*part_info) != 0x20);
    CXLType3Dev *ct3d = container_of(cxl_dstate, CXLType3Dev, cxl_dstate);

    if ((!QEMU_IS_ALIGNED(cxl_dstate->vmem_size, CXL_CAPACITY_MULTIPLIER)) ||
        (!QEMU_IS_ALIGNED(cxl_dstate->pmem_size, CXL_CAPACITY_MULTIPLIER)) ||
        (!QEMU_IS_ALIGNED(ct3d->dc.total_capacity, CXL_CAPACITY_MULTIPLIER))) {
        return CXL_MBOX_INTERNAL_ERROR;
    }

    stq_le_p(&part_info->active_vmem,
             cxl_dstate->vmem_size / CXL_CAPACITY_MULTIPLIER);
    /*
     * When both next_vmem and next_pmem are 0, there is no pending change to
     * partitioning.
     */
    stq_le_p(&part_info->next_vmem, 0);
    stq_le_p(&part_info->active_pmem,
             cxl_dstate->pmem_size / CXL_CAPACITY_MULTIPLIER);
    stq_le_p(&part_info->next_pmem, 0);

    *len_out = sizeof(*part_info);
    return CXL_MBOX_SUCCESS;
}

static CXLRetCode cmd_ccls_get_lsa(const struct cxl_cmd *cmd,
                                   uint8_t *payload_in,
                                   size_t len_in,
                                   uint8_t *payload_out,
                                   size_t *len_out,
                                   CXLCCI *cci)
{
    struct {
        uint32_t offset;
        uint32_t length;
    } QEMU_PACKED *get_lsa;
    CXLType3Dev *ct3d = CXL_TYPE3(cci->d);
    CXLType3Class *cvc = CXL_TYPE3_GET_CLASS(ct3d);
    uint32_t offset, length;

    get_lsa = (void *)payload_in;
    offset = get_lsa->offset;
    length = get_lsa->length;

    if (offset + length > cvc->get_lsa_size(ct3d)) {
        *len_out = 0;
        return CXL_MBOX_INVALID_INPUT;
    }

    *len_out = cvc->get_lsa(ct3d, payload_out, length, offset);
    return CXL_MBOX_SUCCESS;
}

static CXLRetCode cmd_ccls_set_lsa(const struct cxl_cmd *cmd,
                                   uint8_t *payload_in,
                                   size_t len_in,
                                   uint8_t *payload_out,
                                   size_t *len_out,
                                   CXLCCI *cci)
{
    struct set_lsa_pl {
        uint32_t offset;
        uint32_t rsvd;
        uint8_t data[];
    } QEMU_PACKED;
    struct set_lsa_pl *set_lsa_payload = (void *)payload_in;
    CXLType3Dev *ct3d = CXL_TYPE3(cci->d);
    CXLType3Class *cvc = CXL_TYPE3_GET_CLASS(ct3d);
    const size_t hdr_len = offsetof(struct set_lsa_pl, data);

    *len_out = 0;
    if (!len_in) {
        return CXL_MBOX_SUCCESS;
    }

    if (set_lsa_payload->offset + len_in > cvc->get_lsa_size(ct3d) + hdr_len) {
        return CXL_MBOX_INVALID_INPUT;
    }
    len_in -= hdr_len;

    cvc->set_lsa(ct3d, set_lsa_payload->data, len_in, set_lsa_payload->offset);
    return CXL_MBOX_SUCCESS;
}

/* Perform the actual device zeroing */
static void __do_sanitization(CXLType3Dev *ct3d)
{
    MemoryRegion *mr;

    if (ct3d->hostvmem) {
        mr = host_memory_backend_get_memory(ct3d->hostvmem);
        if (mr) {
            void *hostmem = memory_region_get_ram_ptr(mr);
            memset(hostmem, 0, memory_region_size(mr));
        }
    }

    if (ct3d->hostpmem) {
        mr = host_memory_backend_get_memory(ct3d->hostpmem);
        if (mr) {
            void *hostmem = memory_region_get_ram_ptr(mr);
            memset(hostmem, 0, memory_region_size(mr));
        }
    }
    if (ct3d->lsa) {
        mr = host_memory_backend_get_memory(ct3d->lsa);
        if (mr) {
            void *lsa = memory_region_get_ram_ptr(mr);
            memset(lsa, 0, memory_region_size(mr));
        }
    }
}

/*
 * CXL 3.0 spec section 8.2.9.8.5.1 - Sanitize.
 *
 * Once the Sanitize command has started successfully, the device shall be
 * placed in the media disabled state. If the command fails or is interrupted
 * by a reset or power failure, it shall remain in the media disabled state
 * until a successful Sanitize command has been completed. During this state:
 *
 * 1. Memory writes to the device will have no effect, and all memory reads
 * will return random values (no user data returned, even for locations that
 * the failed Sanitize operation didn’t sanitize yet).
 *
 * 2. Mailbox commands shall still be processed in the disabled state, except
 * that commands that access Sanitized areas shall fail with the Media Disabled
 * error code.
 */
static CXLRetCode cmd_sanitize_overwrite(const struct cxl_cmd *cmd,
                                         uint8_t *payload_in,
                                         size_t len_in,
                                         uint8_t *payload_out,
                                         size_t *len_out,
                                         CXLCCI *cci)
{
    CXLType3Dev *ct3d = CXL_TYPE3(cci->d);
    uint64_t total_mem; /* in Mb */
    int secs;

    total_mem = (ct3d->cxl_dstate.vmem_size + ct3d->cxl_dstate.pmem_size) >> 20;
    if (total_mem <= 512) {
        secs = 4;
    } else if (total_mem <= 1024) {
        secs = 8;
    } else if (total_mem <= 2 * 1024) {
        secs = 15;
    } else if (total_mem <= 4 * 1024) {
        secs = 30;
    } else if (total_mem <= 8 * 1024) {
        secs = 60;
    } else if (total_mem <= 16 * 1024) {
        secs = 2 * 60;
    } else if (total_mem <= 32 * 1024) {
        secs = 4 * 60;
    } else if (total_mem <= 64 * 1024) {
        secs = 8 * 60;
    } else if (total_mem <= 128 * 1024) {
        secs = 15 * 60;
    } else if (total_mem <= 256 * 1024) {
        secs = 30 * 60;
    } else if (total_mem <= 512 * 1024) {
        secs = 60 * 60;
    } else if (total_mem <= 1024 * 1024) {
        secs = 120 * 60;
    } else {
        secs = 240 * 60; /* max 4 hrs */
    }

    /* EBUSY other bg cmds as of now */
    cci->bg.runtime = secs * 1000UL;
    *len_out = 0;

    qemu_log_mask(LOG_UNIMP,
                  "Sanitize/overwrite command runtime for %" PRIu64 "Mb media: %d seconds\n",
                  total_mem, secs);

    cxl_dev_disable_media(&ct3d->cxl_dstate);

    if (secs > 2) {
        /* sanitize when done */
        return CXL_MBOX_BG_STARTED;
    } else {
        __do_sanitization(ct3d);
        cxl_dev_enable_media(&ct3d->cxl_dstate);

        return CXL_MBOX_SUCCESS;
    }
}

static CXLRetCode cmd_get_security_state(const struct cxl_cmd *cmd,
                                         uint8_t *payload_in,
                                         size_t len_in,
                                         uint8_t *payload_out,
                                         size_t *len_out,
                                         CXLCCI *cci)
{
    uint32_t *state = (uint32_t *)payload_out;

    *state = 0;
    *len_out = 4;
    return CXL_MBOX_SUCCESS;
}
/*
 * This is very inefficient, but good enough for now!
 * Also the payload will always fit, so no need to handle the MORE flag and
 * make this stateful. We may want to allow longer poison lists to aid
 * testing that kernel functionality.
 */
static CXLRetCode cmd_media_get_poison_list(const struct cxl_cmd *cmd,
                                            uint8_t *payload_in,
                                            size_t len_in,
                                            uint8_t *payload_out,
                                            size_t *len_out,
                                            CXLCCI *cci)
{
    struct get_poison_list_pl {
        uint64_t pa;
        uint64_t length;
    } QEMU_PACKED;

    struct get_poison_list_out_pl {
        uint8_t flags;
        uint8_t rsvd1;
        uint64_t overflow_timestamp;
        uint16_t count;
        uint8_t rsvd2[0x14];
        struct {
            uint64_t addr;
            uint32_t length;
            uint32_t resv;
        } QEMU_PACKED records[];
    } QEMU_PACKED;

    struct get_poison_list_pl *in = (void *)payload_in;
    struct get_poison_list_out_pl *out = (void *)payload_out;
    CXLType3Dev *ct3d = CXL_TYPE3(cci->d);
    uint16_t record_count = 0, i = 0;
    uint64_t query_start, query_length;
    CXLPoisonList *poison_list = &ct3d->poison_list;
    CXLPoison *ent;
    uint16_t out_pl_len;

    query_start = ldq_le_p(&in->pa);
    /* 64 byte alignemnt required */
    if (query_start & 0x3f) {
        return CXL_MBOX_INVALID_INPUT;
    }
    query_length = ldq_le_p(&in->length) * CXL_CACHE_LINE_SIZE;

    QLIST_FOREACH(ent, poison_list, node) {
        /* Check for no overlap */
        if (ent->start >= query_start + query_length ||
            ent->start + ent->length <= query_start) {
            continue;
        }
        record_count++;
    }
    out_pl_len = sizeof(*out) + record_count * sizeof(out->records[0]);
    assert(out_pl_len <= CXL_MAILBOX_MAX_PAYLOAD_SIZE);

    memset(out, 0, out_pl_len);
    QLIST_FOREACH(ent, poison_list, node) {
        uint64_t start, stop;

        /* Check for no overlap */
        if (ent->start >= query_start + query_length ||
            ent->start + ent->length <= query_start) {
            continue;
        }

        /* Deal with overlap */
        start = MAX(ROUND_DOWN(ent->start, 64ull), query_start);
        stop = MIN(ROUND_DOWN(ent->start, 64ull) + ent->length,
                   query_start + query_length);
        stq_le_p(&out->records[i].addr, start | (ent->type & 0x7));
        stl_le_p(&out->records[i].length, (stop - start) / CXL_CACHE_LINE_SIZE);
        i++;
    }
    if (ct3d->poison_list_overflowed) {
        out->flags = (1 << 1);
        stq_le_p(&out->overflow_timestamp, ct3d->poison_list_overflow_ts);
    }
    if (scan_media_running(cci)) {
        out->flags |= (1 << 2);
    }

    stw_le_p(&out->count, record_count);
    *len_out = out_pl_len;
    return CXL_MBOX_SUCCESS;
}

static CXLRetCode cmd_media_inject_poison(const struct cxl_cmd *cmd,
                                          uint8_t *payload_in,
                                          size_t len_in,
                                          uint8_t *payload_out,
                                          size_t *len_out,
                                          CXLCCI *cci)
{
    CXLType3Dev *ct3d = CXL_TYPE3(cci->d);
    CXLPoisonList *poison_list = &ct3d->poison_list;
    CXLPoison *ent;
    struct inject_poison_pl {
        uint64_t dpa;
    };
    struct inject_poison_pl *in = (void *)payload_in;
    uint64_t dpa = ldq_le_p(&in->dpa);
    CXLPoison *p;

    QLIST_FOREACH(ent, poison_list, node) {
        if (dpa >= ent->start &&
            dpa + CXL_CACHE_LINE_SIZE <= ent->start + ent->length) {
            return CXL_MBOX_SUCCESS;
        }
    }
    /*
     * Freeze the list if there is an on-going scan media operation.
     */
    if (scan_media_running(cci)) {
        /*
         * XXX: Spec is ambiguous - is this case considered
         * a successful return despite not adding to the list?
         */
        goto success;
    }

    if (ct3d->poison_list_cnt == CXL_POISON_LIST_LIMIT) {
        return CXL_MBOX_INJECT_POISON_LIMIT;
    }
    p = g_new0(CXLPoison, 1);

    p->length = CXL_CACHE_LINE_SIZE;
    p->start = dpa;
    p->type = CXL_POISON_TYPE_INJECTED;

    /*
     * Possible todo: Merge with existing entry if next to it and if same type
     */
    QLIST_INSERT_HEAD(poison_list, p, node);
    ct3d->poison_list_cnt++;
success:
    *len_out = 0;

    return CXL_MBOX_SUCCESS;
}

static CXLRetCode cmd_media_clear_poison(const struct cxl_cmd *cmd,
                                         uint8_t *payload_in,
                                         size_t len_in,
                                         uint8_t *payload_out,
                                         size_t *len_out,
                                         CXLCCI *cci)
{
    CXLType3Dev *ct3d = CXL_TYPE3(cci->d);
    CXLDeviceState *cxl_dstate = &ct3d->cxl_dstate;
    CXLPoisonList *poison_list = &ct3d->poison_list;
    CXLType3Class *cvc = CXL_TYPE3_GET_CLASS(ct3d);
    struct clear_poison_pl {
        uint64_t dpa;
        uint8_t data[64];
    };
    CXLPoison *ent;
    uint64_t dpa;

    struct clear_poison_pl *in = (void *)payload_in;

    dpa = ldq_le_p(&in->dpa);
    if (dpa + CXL_CACHE_LINE_SIZE >= cxl_dstate->static_mem_size
            && ct3d->dc.num_regions == 0) {
        return CXL_MBOX_INVALID_PA;
    }

    if (ct3d->dc.num_regions && dpa + CXL_CACHE_LINE_SIZE >=
            cxl_dstate->static_mem_size + ct3d->dc.total_capacity) {
        return CXL_MBOX_INVALID_PA;
    }

    /* Clearing a region with no poison is not an error so always do so */
    if (cvc->set_cacheline) {
        if (!cvc->set_cacheline(ct3d, dpa, in->data)) {
            return CXL_MBOX_INTERNAL_ERROR;
        }
    }

    /*
     * Freeze the list if there is an on-going scan media operation.
     */
    if (scan_media_running(cci)) {
        /*
         * XXX: Spec is ambiguous - is this case considered
         * a successful return despite not removing from the list?
         */
        goto success;
    }

    QLIST_FOREACH(ent, poison_list, node) {
        /*
         * Test for contained in entry. Simpler than general case
         * as clearing 64 bytes and entries 64 byte aligned
         */
        if ((dpa >= ent->start) && (dpa < ent->start + ent->length)) {
            break;
        }
    }
    if (!ent) {
        goto success;
    }

    QLIST_REMOVE(ent, node);
    ct3d->poison_list_cnt--;

    if (dpa > ent->start) {
        CXLPoison *frag;
        /* Cannot overflow as replacing existing entry */

        frag = g_new0(CXLPoison, 1);

        frag->start = ent->start;
        frag->length = dpa - ent->start;
        frag->type = ent->type;

        QLIST_INSERT_HEAD(poison_list, frag, node);
        ct3d->poison_list_cnt++;
    }

    if (dpa + CXL_CACHE_LINE_SIZE < ent->start + ent->length) {
        CXLPoison *frag;

        if (ct3d->poison_list_cnt == CXL_POISON_LIST_LIMIT) {
            cxl_set_poison_list_overflowed(ct3d);
        } else {
            frag = g_new0(CXLPoison, 1);

            frag->start = dpa + CXL_CACHE_LINE_SIZE;
            frag->length = ent->start + ent->length - frag->start;
            frag->type = ent->type;
            QLIST_INSERT_HEAD(poison_list, frag, node);
            ct3d->poison_list_cnt++;
        }
    }
    /* Any fragments have been added, free original entry */
    g_free(ent);
success:
    *len_out = 0;

    return CXL_MBOX_SUCCESS;
}

/*
 * CXL r3.0 section 8.2.9.8.4.4: Get Scan Media Capabilities
 */
static CXLRetCode
cmd_media_get_scan_media_capabilities(const struct cxl_cmd *cmd,
                                      uint8_t *payload_in,
                                      size_t len_in,
                                      uint8_t *payload_out,
                                      size_t *len_out,
                                      CXLCCI *cci)
{
    struct get_scan_media_capabilities_pl {
        uint64_t pa;
        uint64_t length;
    } QEMU_PACKED;

    struct get_scan_media_capabilities_out_pl {
        uint32_t estimated_runtime_ms;
    } QEMU_PACKED;

    CXLType3Dev *ct3d = CXL_TYPE3(cci->d);
    CXLDeviceState *cxl_dstate = &ct3d->cxl_dstate;
    struct get_scan_media_capabilities_pl *in = (void *)payload_in;
    struct get_scan_media_capabilities_out_pl *out = (void *)payload_out;
    uint64_t query_start;
    uint64_t query_length;

    query_start = ldq_le_p(&in->pa);
    /* 64 byte alignment required */
    if (query_start & 0x3f) {
        return CXL_MBOX_INVALID_INPUT;
    }
    query_length = ldq_le_p(&in->length) * CXL_CACHE_LINE_SIZE;

    if (query_start + query_length > cxl_dstate->static_mem_size) {
        return CXL_MBOX_INVALID_PA;
    }

    /*
     * Just use 400 nanosecond access/read latency + 100 ns for
     * the cost of updating the poison list. For small enough
     * chunks return at least 1 ms.
     */
    stl_le_p(&out->estimated_runtime_ms,
             MAX(1, query_length * (0.0005L / 64)));

    *len_out = sizeof(*out);
    return CXL_MBOX_SUCCESS;
}

static void __do_scan_media(CXLType3Dev *ct3d)
{
    CXLPoison *ent;
    unsigned int results_cnt = 0;

    QLIST_FOREACH(ent, &ct3d->scan_media_results, node) {
        results_cnt++;
    }

    /* only scan media may clear the overflow */
    if (ct3d->poison_list_overflowed &&
        ct3d->poison_list_cnt == results_cnt) {
        cxl_clear_poison_list_overflowed(ct3d);
    }
    /* scan media has run since last conventional reset */
    ct3d->scan_media_hasrun = true;
}

/*
 * CXL r3.0 section 8.2.9.8.4.5: Scan Media
 */
static CXLRetCode cmd_media_scan_media(const struct cxl_cmd *cmd,
                                       uint8_t *payload_in,
                                       size_t len_in,
                                       uint8_t *payload_out,
                                       size_t *len_out,
                                       CXLCCI *cci)
{
    struct scan_media_pl {
        uint64_t pa;
        uint64_t length;
        uint8_t flags;
    } QEMU_PACKED;

    struct scan_media_pl *in = (void *)payload_in;
    CXLType3Dev *ct3d = CXL_TYPE3(cci->d);
    CXLDeviceState *cxl_dstate = &ct3d->cxl_dstate;
    uint64_t query_start;
    uint64_t query_length;
    CXLPoison *ent, *next;

    query_start = ldq_le_p(&in->pa);
    /* 64 byte alignment required */
    if (query_start & 0x3f) {
        return CXL_MBOX_INVALID_INPUT;
    }
    query_length = ldq_le_p(&in->length) * CXL_CACHE_LINE_SIZE;

    if (query_start + query_length > cxl_dstate->static_mem_size) {
        return CXL_MBOX_INVALID_PA;
    }
    if (ct3d->dc.num_regions && query_start + query_length >=
            cxl_dstate->static_mem_size + ct3d->dc.total_capacity) {
        return CXL_MBOX_INVALID_PA;
    }

    if (in->flags == 0) { /* TODO */
        qemu_log_mask(LOG_UNIMP,
                      "Scan Media Event Log is unsupported\n");
    }

    /* any previous results are discarded upon a new Scan Media */
    QLIST_FOREACH_SAFE(ent, &ct3d->scan_media_results, node, next) {
        QLIST_REMOVE(ent, node);
        g_free(ent);
    }

    /* kill the poison list - it will be recreated */
    if (ct3d->poison_list_overflowed) {
        QLIST_FOREACH_SAFE(ent, &ct3d->poison_list, node, next) {
            QLIST_REMOVE(ent, node);
            g_free(ent);
            ct3d->poison_list_cnt--;
        }
    }

    /*
     * Scan the backup list and move corresponding entries
     * into the results list, updating the poison list
     * when possible.
     */
    QLIST_FOREACH_SAFE(ent, &ct3d->poison_list_bkp, node, next) {
        CXLPoison *res;

        if (ent->start >= query_start + query_length ||
            ent->start + ent->length <= query_start) {
            continue;
        }

        /*
         * If a Get Poison List cmd comes in while this
         * scan is being done, it will see the new complete
         * list, while setting the respective flag.
         */
        if (ct3d->poison_list_cnt < CXL_POISON_LIST_LIMIT) {
            CXLPoison *p = g_new0(CXLPoison, 1);

            p->start = ent->start;
            p->length = ent->length;
            p->type = ent->type;
            QLIST_INSERT_HEAD(&ct3d->poison_list, p, node);
            ct3d->poison_list_cnt++;
        }

        res = g_new0(CXLPoison, 1);
        res->start = ent->start;
        res->length = ent->length;
        res->type = ent->type;
        QLIST_INSERT_HEAD(&ct3d->scan_media_results, res, node);

        QLIST_REMOVE(ent, node);
        g_free(ent);
    }

    cci->bg.runtime = MAX(1, query_length * (0.0005L / 64));
    *len_out = 0;

    return CXL_MBOX_BG_STARTED;
}

/*
 * CXL r3.0 section 8.2.9.8.4.6: Get Scan Media Results
 */
static CXLRetCode cmd_media_get_scan_media_results(const struct cxl_cmd *cmd,
                                                   uint8_t *payload_in,
                                                   size_t len_in,
                                                   uint8_t *payload_out,
                                                   size_t *len_out,
                                                   CXLCCI *cci)
{
    struct get_scan_media_results_out_pl {
        uint64_t dpa_restart;
        uint64_t length;
        uint8_t flags;
        uint8_t rsvd1;
        uint16_t count;
        uint8_t rsvd2[0xc];
        struct {
            uint64_t addr;
            uint32_t length;
            uint32_t resv;
        } QEMU_PACKED records[];
    } QEMU_PACKED;

    struct get_scan_media_results_out_pl *out = (void *)payload_out;
    CXLType3Dev *ct3d = CXL_TYPE3(cci->d);
    CXLPoisonList *scan_media_results = &ct3d->scan_media_results;
    CXLPoison *ent, *next;
    uint16_t total_count = 0, record_count = 0, i = 0;
    uint16_t out_pl_len;

    if (!ct3d->scan_media_hasrun) {
        return CXL_MBOX_UNSUPPORTED;
    }

    /*
     * Calculate limits, all entries are within the same
     * address range of the last scan media call.
     */
    QLIST_FOREACH(ent, scan_media_results, node) {
        size_t rec_size = record_count * sizeof(out->records[0]);

        if (sizeof(*out) + rec_size < CXL_MAILBOX_MAX_PAYLOAD_SIZE) {
            record_count++;
        }
        total_count++;
    }

    out_pl_len = sizeof(*out) + record_count * sizeof(out->records[0]);
    assert(out_pl_len <= CXL_MAILBOX_MAX_PAYLOAD_SIZE);

    memset(out, 0, out_pl_len);
    QLIST_FOREACH_SAFE(ent, scan_media_results, node, next) {
        uint64_t start, stop;

        if (i == record_count) {
            break;
        }

        start = ROUND_DOWN(ent->start, 64ull);
        stop = ROUND_DOWN(ent->start, 64ull) + ent->length;
        stq_le_p(&out->records[i].addr, start | (ent->type & 0x7));
        stl_le_p(&out->records[i].length, (stop - start) / CXL_CACHE_LINE_SIZE);
        i++;

        /* consume the returning entry */
        QLIST_REMOVE(ent, node);
        g_free(ent);
    }

    stw_le_p(&out->count, record_count);
    if (total_count > record_count) {
        out->flags = (1 << 0); /* More Media Error Records */
    }

    *len_out = out_pl_len;
    return CXL_MBOX_SUCCESS;
}

/*
 * CXL r3.0 section 8.2.9.8.9.1: Dynamic Capacity Configuration
 */
static CXLRetCode cmd_dcd_get_dyn_cap_config(const struct cxl_cmd *cmd,
                                             uint8_t *payload_in,
                                             size_t len_in,
                                             uint8_t *payload_out,
                                             size_t *len_out,
                                             CXLCCI *cci)
{
    CXLType3Dev *ct3d = CXL_TYPE3(cci->d);
    struct get_dyn_cap_config_in_pl {
        uint8_t region_cnt;
        uint8_t start_region_id;
    } QEMU_PACKED;

    struct get_dyn_cap_config_out_pl {
        uint8_t num_regions;
        uint8_t rsvd1[7];
        struct {
            uint64_t base;
            uint64_t decode_len;
            uint64_t region_len;
            uint64_t block_size;
            uint32_t dsmadhandle;
            uint8_t flags;
            uint8_t rsvd2[3];
        } QEMU_PACKED records[];
    } QEMU_PACKED;

    struct get_dyn_cap_config_in_pl *in = (void *)payload_in;
    struct get_dyn_cap_config_out_pl *out = (void *)payload_out;
    uint16_t record_count = 0, i;
    uint16_t out_pl_len;
    uint8_t start_region_id = in->start_region_id;

    if (start_region_id >= ct3d->dc.num_regions) {
        return CXL_MBOX_INVALID_INPUT;
    }

    record_count = MIN(ct3d->dc.num_regions - in->start_region_id,
            in->region_cnt);

    out_pl_len = sizeof(*out) + record_count * sizeof(out->records[0]);
    assert(out_pl_len <= CXL_MAILBOX_MAX_PAYLOAD_SIZE);

    memset(out, 0, out_pl_len);
    out->num_regions = record_count;
    for (i = 0; i < record_count; i++) {
        stq_le_p(&out->records[i].base,
                ct3d->dc.regions[start_region_id + i].base);
        stq_le_p(&out->records[i].decode_len,
                ct3d->dc.regions[start_region_id + i].decode_len);
        stq_le_p(&out->records[i].region_len,
                ct3d->dc.regions[start_region_id + i].len);
        stq_le_p(&out->records[i].block_size,
                ct3d->dc.regions[start_region_id + i].block_size);
        stl_le_p(&out->records[i].dsmadhandle,
                ct3d->dc.regions[start_region_id + i].dsmadhandle);
        out->records[i].flags = ct3d->dc.regions[start_region_id + i].flags;
    }

    *len_out = out_pl_len;
    return CXL_MBOX_SUCCESS;
}

/*
 * CXL r3.0 section 8.2.9.8.9.2:
 * Get Dynamic Capacity Extent List (Opcode 4810h)
 */
static CXLRetCode cmd_dcd_get_dyn_cap_ext_list(const struct cxl_cmd *cmd,
                                               uint8_t *payload_in,
                                               size_t len_in,
                                               uint8_t *payload_out,
                                               size_t *len_out,
                                               CXLCCI *cci)
{
    CXLType3Dev *ct3d = CXL_TYPE3(cci->d);
    struct get_dyn_cap_ext_list_in_pl {
        uint32_t extent_cnt;
        uint32_t start_extent_id;
    } QEMU_PACKED;

    struct get_dyn_cap_ext_list_out_pl {
        uint32_t count;
        uint32_t total_extents;
        uint32_t generation_num;
        uint8_t rsvd[4];
        CXLDCExtentRaw records[];
    } QEMU_PACKED;

    struct get_dyn_cap_ext_list_in_pl *in = (void *)payload_in;
    struct get_dyn_cap_ext_list_out_pl *out = (void *)payload_out;
    uint16_t record_count = 0, i = 0, record_done = 0;
    CXLDCDExtentList *extent_list = &ct3d->dc.extents;
    CXLDCDExtent *ent;
    uint16_t out_pl_len;
    uint32_t start_extent_id = in->start_extent_id;

    if (start_extent_id > ct3d->dc.total_extent_count) {
        return CXL_MBOX_INVALID_INPUT;
    }

    record_count = MIN(in->extent_cnt,
                       ct3d->dc.total_extent_count - start_extent_id);

    out_pl_len = sizeof(*out) + record_count * sizeof(out->records[0]);
    /* May need more processing here in the future */
    assert(out_pl_len <= CXL_MAILBOX_MAX_PAYLOAD_SIZE);

    memset(out, 0, out_pl_len);
    stl_le_p(&out->count, record_count);
    stl_le_p(&out->total_extents, ct3d->dc.total_extent_count);
    stl_le_p(&out->generation_num, ct3d->dc.ext_list_gen_seq);

    if (record_count > 0) {
        QTAILQ_FOREACH(ent, extent_list, node) {
            if (i++ < start_extent_id) {
                continue;
            }
            stq_le_p(&out->records[record_done].start_dpa, ent->start_dpa);
            stq_le_p(&out->records[record_done].len, ent->len);
            memcpy(&out->records[record_done].tag, ent->tag, 0x10);
            stw_le_p(&out->records[record_done].shared_seq, ent->shared_seq);
            record_done++;
            if (record_done == record_count) {
                break;
            }
        }
    }

    *len_out = out_pl_len;
    return CXL_MBOX_SUCCESS;
}

/*
 * Check whether the bits at addr between [nr, nr+size) are all set,
 * return 1 if all 1s, else return 0
 */
static bool test_bits(const unsigned long *addr, int nr, int size)
{
    unsigned long res = find_next_zero_bit(addr, size + nr, nr);

    return res >= nr + size;
}

CXLDCDRegion *cxl_find_dc_region(CXLType3Dev *ct3d, uint64_t dpa, uint64_t len)
{
    CXLDCDRegion *region = &ct3d->dc.regions[0];
    int i;

    if (dpa < region->base ||
        dpa >= region->base + ct3d->dc.total_capacity) {
        return NULL;
    }

    /*
     * CXL r3.0 section 9.13.3: Dynamic Capacity Device (DCD)
     *
     * Regions are used in increasing-DPA order, with Region 0 being used for
     * the lowest DPA of Dynamic Capacity and Region 7 for the highest DPA.
     * So check from the last region to find where the dpa belongs. Extents that
     * cross multiple regions are not allowed.
     */
    for (i = ct3d->dc.num_regions - 1; i >= 0; i--) {
        region = &ct3d->dc.regions[i];
        if (dpa >= region->base) {
            return region;
        }
    }
    return NULL;
}

static void cxl_insert_extent_to_extent_list(CXLDCDExtentList *list,
                                             uint64_t dpa,
                                             uint64_t len,
                                             uint8_t *tag,
                                             uint16_t shared_seq)
{
    CXLDCDExtent *extent;

    extent = g_new0(CXLDCDExtent, 1);
    extent->start_dpa = dpa;
    extent->len = len;
    if (tag) {
        memcpy(extent->tag, tag, 0x10);
    } else {
        memset(extent->tag, 0, 0x10);
    }
    extent->shared_seq = shared_seq;

    QTAILQ_INSERT_TAIL(list, extent, node);
}

/*
 * CXL r3.0 Table 8-129: Add Dynamic Capacity Response Input Payload
 * CXL r3.0 Table 8-131: Release Dynamic Capacity Input Payload
 */
typedef struct updated_dc_extent_list_in_pl {
    uint32_t num_entries_updated;
    uint8_t rsvd[4];
    /* CXL r3.0 Table 8-130: Updated Extent List */
    struct {
        uint64_t start_dpa;
        uint64_t len;
        uint8_t rsvd[8];
    } QEMU_PACKED updated_entries[];
} QEMU_PACKED updated_dc_extent_list_in_pl;

/*
 * The function only check the input extent list against itself.
 */
static CXLRetCode cxl_detect_malformed_extent_list(CXLType3Dev *ct3d,
        const updated_dc_extent_list_in_pl *in)
{
    uint64_t min_block_size = UINT64_MAX;
    CXLDCDRegion *region = &ct3d->dc.regions[0];
    CXLDCDRegion *lastregion = &ct3d->dc.regions[ct3d->dc.num_regions - 1];
    g_autofree unsigned long *blk_bitmap = NULL;
    uint64_t dpa, len;
    uint32_t i;

    for (i = 0; i < ct3d->dc.num_regions; i++) {
        region = &ct3d->dc.regions[i];
        min_block_size = MIN(min_block_size, region->block_size);
    }

    blk_bitmap = bitmap_new((lastregion->len + lastregion->base -
                             ct3d->dc.regions[0].base) / min_block_size);

    for (i = 0; i < in->num_entries_updated; i++) {
        dpa = in->updated_entries[i].start_dpa;
        len = in->updated_entries[i].len;

        region = cxl_find_dc_region(ct3d, dpa, len);
        if (!region) {
            return CXL_MBOX_INVALID_PA;
        }

        if (dpa % region->block_size || len % region->block_size) {
            return CXL_MBOX_INVALID_EXTENT_LIST;
        }
        /* the dpa range already covered by some other extents in the list */
        if (test_bits(blk_bitmap, dpa / min_block_size, len / min_block_size)) {
            return CXL_MBOX_INVALID_EXTENT_LIST;
        }
        bitmap_set(blk_bitmap, dpa / min_block_size, len / min_block_size);
   }

    return CXL_MBOX_SUCCESS;
}

/*
 * CXL r3.0 section 8.2.9.8.9.3: Add Dynamic Capacity Response (opcode 4802h)
 *
 * Assume an extent is added only after the response is processed successfully
 * TODO: for better extent list validation, a better solution would be
 * maintaining a pending extent list and use it to verify the extent list in
 * the response.
 */
static CXLRetCode cmd_dcd_add_dyn_cap_rsp(const struct cxl_cmd *cmd,
                                          uint8_t *payload_in,
                                          size_t len_in,
                                          uint8_t *payload_out,
                                          size_t *len_out,
                                          CXLCCI *cci)
{
    updated_dc_extent_list_in_pl *in = (void *)payload_in;
    CXLType3Dev *ct3d = CXL_TYPE3(cci->d);
    CXLDCDExtentList *extent_list = &ct3d->dc.extents;
    CXLDCDExtent *ent;
    uint32_t i;
    uint64_t dpa, len;
    CXLRetCode ret;

    if (in->num_entries_updated == 0) {
        return CXL_MBOX_SUCCESS;
    }

    ret = cxl_detect_malformed_extent_list(ct3d, in);
    if (ret != CXL_MBOX_SUCCESS) {
        return ret;
    }

    for (i = 0; i < in->num_entries_updated; i++) {
        dpa = in->updated_entries[i].start_dpa;
        len = in->updated_entries[i].len;

        /*
         * Check if the DPA range of the to-be-added extent overlaps with
         * existing extent list maintained by the device.
         * TODO: Minimize set of checks.
         */
        QTAILQ_FOREACH(ent, extent_list, node) {
            /* Exact match */
            if (ent->start_dpa == dpa && ent->len == len) {
                return CXL_MBOX_INVALID_PA;
                /* Subsection of existing extent */
            } else if (ent->start_dpa <= dpa &&
                       dpa + len <= ent->start_dpa + ent->len) {
                return CXL_MBOX_INVALID_PA;
                /* Overlapping one end of the other */
            } else if ((dpa < ent->start_dpa + ent->len &&
                        dpa + len > ent->start_dpa + ent->len) ||
                       (dpa < ent->start_dpa && dpa + len > ent->start_dpa)) {
                return CXL_MBOX_INVALID_PA;
            }
        }

        /*
         * TODO: add a pending extent list based on event log record and verify
         * the input response
         */

        cxl_insert_extent_to_extent_list(extent_list, dpa, len, NULL, 0);
    }

    return CXL_MBOX_SUCCESS;
}

/*
 * CXL r3.0 section 8.2.9.8.9.4: Release Dynamic Capacity (opcode 4803h)
 */
static CXLRetCode cmd_dcd_release_dyn_cap(const struct cxl_cmd *cmd,
                                          uint8_t *payload_in,
                                          size_t len_in,
                                          uint8_t *payload_out,
                                          size_t *len_out,
                                          CXLCCI *cci)
{
    updated_dc_extent_list_in_pl *in = (void *)payload_in;
    CXLType3Dev *ct3d = CXL_TYPE3(cci->d);
    CXLDCDExtentList *extent_list = &ct3d->dc.extents;
    CXLDCDExtent *ent;
    uint32_t i;
    uint64_t dpa, len;
    CXLRetCode ret;

    if (in->num_entries_updated == 0) {
        return CXL_MBOX_INVALID_INPUT;
    }

    ret = cxl_detect_malformed_extent_list(ct3d, in);
    if (ret != CXL_MBOX_SUCCESS) {
        return ret;
    }

    for (i = 0; i < in->num_entries_updated; i++) {
        dpa = in->updated_entries[i].start_dpa;
        len = in->updated_entries[i].len;

        QTAILQ_FOREACH(ent, extent_list, node) {
            if (ent->start_dpa <= dpa &&
                dpa + len <= ent->start_dpa + ent->len) {
                /* Remove any partial extents */
                uint64_t len1 = dpa - ent->start_dpa;
                uint64_t len2 = ent->start_dpa + ent->len - dpa - len;

                if (len1) {
                    cxl_insert_extent_to_extent_list(extent_list,
                                                     ent->start_dpa, len1,
                                                     NULL, 0);
                }
                if (len2) {
                    cxl_insert_extent_to_extent_list(extent_list, dpa + len,
                                                     len2, NULL, 0);
                }
                break;
            } else if ((dpa < ent->start_dpa + ent->len &&
                        dpa + len > ent->start_dpa + ent->len) ||
                       (dpa < ent->start_dpa && dpa + len > ent->start_dpa)) {
                return CXL_MBOX_INVALID_EXTENT_LIST;
            }
        }

        if (ent) {
            QTAILQ_REMOVE(extent_list, ent, node);
            g_free(ent);
        } else {
            /* Try to remove a non-existing extent */
            return CXL_MBOX_INVALID_PA;
        }
    }

    return CXL_MBOX_SUCCESS;
}

static const struct cxl_cmd cxl_cmd_set[256][256] = {
    [EVENTS][GET_RECORDS] = { "EVENTS_GET_RECORDS",
        cmd_events_get_records, 1, 0 },
    [EVENTS][CLEAR_RECORDS] = { "EVENTS_CLEAR_RECORDS",
        cmd_events_clear_records, ~0, CXL_MBOX_IMMEDIATE_LOG_CHANGE },
    [EVENTS][GET_INTERRUPT_POLICY] = { "EVENTS_GET_INTERRUPT_POLICY",
                                      cmd_events_get_interrupt_policy, 0, 0 },
    [EVENTS][SET_INTERRUPT_POLICY] = { "EVENTS_SET_INTERRUPT_POLICY",
                                      cmd_events_set_interrupt_policy,
                                      ~0, CXL_MBOX_IMMEDIATE_CONFIG_CHANGE },
    [FIRMWARE_UPDATE][GET_INFO] = { "FIRMWARE_UPDATE_GET_INFO",
        cmd_firmware_update_get_info, 0, 0 },
    [TIMESTAMP][GET] = { "TIMESTAMP_GET", cmd_timestamp_get, 0, 0 },
    [TIMESTAMP][SET] = { "TIMESTAMP_SET", cmd_timestamp_set,
                         8, CXL_MBOX_IMMEDIATE_POLICY_CHANGE },
    [LOGS][GET_SUPPORTED] = { "LOGS_GET_SUPPORTED", cmd_logs_get_supported,
                              0, 0 },
    [LOGS][GET_LOG] = { "LOGS_GET_LOG", cmd_logs_get_log, 0x18, 0 },
    [IDENTIFY][MEMORY_DEVICE] = { "IDENTIFY_MEMORY_DEVICE",
        cmd_identify_memory_device, 0, 0 },
    [CCLS][GET_PARTITION_INFO] = { "CCLS_GET_PARTITION_INFO",
        cmd_ccls_get_partition_info, 0, 0 },
    [CCLS][GET_LSA] = { "CCLS_GET_LSA", cmd_ccls_get_lsa, 8, 0 },
    [CCLS][SET_LSA] = { "CCLS_SET_LSA", cmd_ccls_set_lsa,
        ~0, CXL_MBOX_IMMEDIATE_CONFIG_CHANGE | CXL_MBOX_IMMEDIATE_DATA_CHANGE },
    [SANITIZE][OVERWRITE] = { "SANITIZE_OVERWRITE", cmd_sanitize_overwrite, 0,
        (CXL_MBOX_IMMEDIATE_DATA_CHANGE |
         CXL_MBOX_SECURITY_STATE_CHANGE |
         CXL_MBOX_BACKGROUND_OPERATION)},
    [PERSISTENT_MEM][GET_SECURITY_STATE] = { "GET_SECURITY_STATE",
        cmd_get_security_state, 0, 0 },
    [MEDIA_AND_POISON][GET_POISON_LIST] = { "MEDIA_AND_POISON_GET_POISON_LIST",
        cmd_media_get_poison_list, 16, 0 },
    [MEDIA_AND_POISON][INJECT_POISON] = { "MEDIA_AND_POISON_INJECT_POISON",
        cmd_media_inject_poison, 8, 0 },
    [MEDIA_AND_POISON][CLEAR_POISON] = { "MEDIA_AND_POISON_CLEAR_POISON",
        cmd_media_clear_poison, 72, 0 },
    [MEDIA_AND_POISON][GET_SCAN_MEDIA_CAPABILITIES] = {
        "MEDIA_AND_POISON_GET_SCAN_MEDIA_CAPABILITIES",
        cmd_media_get_scan_media_capabilities, 16, 0 },
    [MEDIA_AND_POISON][SCAN_MEDIA] = { "MEDIA_AND_POISON_SCAN_MEDIA",
        cmd_media_scan_media, 17, CXL_MBOX_BACKGROUND_OPERATION },
    [MEDIA_AND_POISON][GET_SCAN_MEDIA_RESULTS] = {
        "MEDIA_AND_POISON_GET_SCAN_MEDIA_RESULTS",
        cmd_media_get_scan_media_results, 0, 0 },
    [MHD][GET_MHD_INFO] = { "GET_MULTI_HEADED_INFO", cmd_mhd_get_info, 2, 0},
};

static const struct cxl_cmd cxl_cmd_set_dcd[256][256] = {
    [DCD_CONFIG][GET_DC_CONFIG] = { "DCD_GET_DC_CONFIG",
        cmd_dcd_get_dyn_cap_config, 2, 0 },
    [DCD_CONFIG][GET_DYN_CAP_EXT_LIST] = {
        "DCD_GET_DYNAMIC_CAPACITY_EXTENT_LIST", cmd_dcd_get_dyn_cap_ext_list,
        8, 0 },
    [DCD_CONFIG][ADD_DYN_CAP_RSP] = {
        "ADD_DCD_DYNAMIC_CAPACITY_RESPONSE", cmd_dcd_add_dyn_cap_rsp,
        ~0, CXL_MBOX_IMMEDIATE_DATA_CHANGE },
    [DCD_CONFIG][RELEASE_DYN_CAP] = {
        "RELEASE_DCD_DYNAMIC_CAPACITY", cmd_dcd_release_dyn_cap,
        ~0, CXL_MBOX_IMMEDIATE_DATA_CHANGE },
};

static const struct cxl_cmd cxl_cmd_set_sw[256][256] = {
    [INFOSTAT][IS_IDENTIFY] = { "IDENTIFY", cmd_infostat_identify, 0, 18 },
    [INFOSTAT][BACKGROUND_OPERATION_STATUS] = { "BACKGROUND_OPERATION_STATUS",
        cmd_infostat_bg_op_sts, 0, 8 },
    /*
     * TODO get / set response message limit - requires all messages over
     * 256 bytes to support chunking.
     */
    [TIMESTAMP][GET] = { "TIMESTAMP_GET", cmd_timestamp_get, 0, 0 },
    [TIMESTAMP][SET] = { "TIMESTAMP_SET", cmd_timestamp_set, 8,
                         CXL_MBOX_IMMEDIATE_POLICY_CHANGE },
    [LOGS][GET_SUPPORTED] = { "LOGS_GET_SUPPORTED", cmd_logs_get_supported, 0,
                              0 },
    [LOGS][GET_LOG] = { "LOGS_GET_LOG", cmd_logs_get_log, 0x18, 0 },
    [PHYSICAL_SWITCH][IDENTIFY_SWITCH_DEVICE] = {"IDENTIFY_SWITCH_DEVICE",
        cmd_identify_switch_device, 0, 0x49 },
    [PHYSICAL_SWITCH][GET_PHYSICAL_PORT_STATE] = { "SWITCH_PHYSICAL_PORT_STATS",
        cmd_get_physical_port_state, ~0, ~0 },
    [TUNNEL][MANAGEMENT_COMMAND] = { "TUNNEL_MANAGEMENT_COMMAND",
                                     cmd_tunnel_management_cmd, ~0, ~0 },
};

/*
 * While the command is executing in the background, the device should
 * update the percentage complete in the Background Command Status Register
 * at least once per second.
 */

#define CXL_MBOX_BG_UPDATE_FREQ 1000UL

int cxl_process_cci_message(CXLCCI *cci, uint8_t set, uint8_t cmd,
                            size_t len_in, uint8_t *pl_in, size_t *len_out,
                            uint8_t *pl_out, bool *bg_started)

{
    int ret;
    const struct cxl_cmd *cxl_cmd;
    opcode_handler h;

    cxl_cmd = &cci->cxl_cmd_set[set][cmd];
    h = cxl_cmd->handler;
    if (!h) {
        qemu_log_mask(LOG_UNIMP, "Command %04xh not implemented\n",
                      set << 8 | cmd);
        return CXL_MBOX_UNSUPPORTED;
    }

    if (len_in != cxl_cmd->in && cxl_cmd->in != ~0) {
        return CXL_MBOX_INVALID_PAYLOAD_LENGTH;
    }

    /* Only one bg command at a time */
    if ((cxl_cmd->effect & CXL_MBOX_BACKGROUND_OPERATION) &&
        cci->bg.runtime > 0) {
        return CXL_MBOX_BUSY;
    }

    /* forbid any selected commands while overwriting */
    if (sanitize_running(cci)) {
        if (h == cmd_events_get_records ||
            h == cmd_ccls_get_partition_info ||
            h == cmd_ccls_set_lsa ||
            h == cmd_ccls_get_lsa ||
            h == cmd_logs_get_log ||
            h == cmd_media_get_poison_list ||
            h == cmd_media_inject_poison ||
            h == cmd_media_clear_poison ||
            h == cmd_sanitize_overwrite) {
            return CXL_MBOX_MEDIA_DISABLED;
        }
    }

    ret = (*h)(cxl_cmd, pl_in, len_in, pl_out, len_out, cci);
    if ((cxl_cmd->effect & CXL_MBOX_BACKGROUND_OPERATION) &&
        ret == CXL_MBOX_BG_STARTED) {
        *bg_started = true;
    } else {
        *bg_started = false;
    }

    /* Set bg and the return code */
    /* Right place? - may be a race */
    if (*bg_started) {
        uint64_t now;

        cci->bg.opcode = (set << 8) | cmd;

        cci->bg.complete_pct = 0;
        cci->bg.ret_code = 0;

        now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
        cci->bg.starttime = now;
        timer_mod(cci->bg.timer, now + CXL_MBOX_BG_UPDATE_FREQ);
    }

    return ret;
}

static void bg_timercb(void *opaque)
{
    CXLCCI *cci = opaque;
    uint64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    uint64_t total_time = cci->bg.starttime + cci->bg.runtime;

    assert(cci->bg.runtime > 0);

    if (now >= total_time) { /* we are done */
        uint16_t ret = CXL_MBOX_SUCCESS;

        cci->bg.complete_pct = 100;
        cci->bg.ret_code = ret;
        if (ret == CXL_MBOX_SUCCESS) {
            CXLType3Dev *ct3d = CXL_TYPE3(cci->d);

            switch (cci->bg.opcode) {
            case 0x4400: /* sanitize */
                __do_sanitization(ct3d);
                cxl_dev_enable_media(&ct3d->cxl_dstate);
                break;
            case 0x4304: /* scan media */
                __do_scan_media(ct3d);
                break;
            default:
                __builtin_unreachable();
                break;
            }
        }

        qemu_log("Background command %04xh finished: %s\n",
                 cci->bg.opcode,
                 ret == CXL_MBOX_SUCCESS ? "success" : "aborted");
    } else {
        /* estimate only */
        cci->bg.complete_pct = 100 * now / total_time;
        timer_mod(cci->bg.timer, now + CXL_MBOX_BG_UPDATE_FREQ);
    }

    if (cci->bg.complete_pct == 100) {
        /* FIXME generalize to switch CCI */
        CXLType3Dev *ct3d = CXL_TYPE3(cci->d);
        CXLDeviceState *cxl_dstate = &ct3d->cxl_dstate;
        PCIDevice *pdev = PCI_DEVICE(cci->d);

        cci->bg.starttime = 0;
        /* registers are updated, allow new bg-capable cmds */
        cci->bg.runtime = 0;

        if (msix_enabled(pdev)) {
            msix_notify(pdev, cxl_dstate->mbox_msi_n);
        } else if (msi_enabled(pdev)) {
            msi_notify(pdev, cxl_dstate->mbox_msi_n);
        }
    }
}

static void cxl_rebuild_cel(CXLCCI *cci)
{
    cci->cel_size = 0; /* Reset for a fresh build */
    for (int set = 0; set < 256; set++) {
        for (int cmd = 0; cmd < 256; cmd++) {
            if (cci->cxl_cmd_set[set][cmd].handler) {
                const struct cxl_cmd *c = &cci->cxl_cmd_set[set][cmd];
                struct cel_log *log =
                    &cci->cel_log[cci->cel_size];

                log->opcode = (set << 8) | cmd;
                log->effect = c->effect;
                cci->cel_size++;
            }
        }
    }
}

void cxl_init_cci(CXLCCI *cci, size_t payload_max)
{
    cci->payload_max = payload_max;
    cxl_rebuild_cel(cci);

    cci->bg.complete_pct = 0;
    cci->bg.starttime = 0;
    cci->bg.runtime = 0;
    cci->bg.timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                 bg_timercb, cci);
}

static void cxl_copy_cci_commands(CXLCCI *cci, const struct cxl_cmd (*cxl_cmds)[256])
{
    for (int set = 0; set < 256; set++) {
        for (int cmd = 0; cmd < 256; cmd++) {
            if (cxl_cmds[set][cmd].handler) {
                cci->cxl_cmd_set[set][cmd] = cxl_cmds[set][cmd];
            }
        }
    }
}

void cxl_add_cci_commands(CXLCCI *cci, const struct cxl_cmd (*cxl_cmd_set)[256],
                                 size_t payload_max)
{
    cci->payload_max = payload_max > cci->payload_max ? payload_max : cci->payload_max;
    cxl_copy_cci_commands(cci, cxl_cmd_set);
    cxl_rebuild_cel(cci);
}

void cxl_initialize_mailbox_swcci(CXLCCI *cci, DeviceState *intf,
                                  DeviceState *d, size_t payload_max)
{
    cxl_copy_cci_commands(cci, cxl_cmd_set_sw);
    cci->d = d;
    cci->intf = intf;
    cxl_init_cci(cci, payload_max);
}

void cxl_initialize_mailbox_t3(CXLCCI *cci, DeviceState *d, size_t payload_max)
{
    CXLType3Dev *ct3d = CXL_TYPE3(d);

    cxl_copy_cci_commands(cci, cxl_cmd_set);
    if (ct3d->dc.num_regions) {
        cxl_copy_cci_commands(cci, cxl_cmd_set_dcd);
    }
    cci->d = d;

    /* No separation for PCI MB as protocol handled in PCI device */
    cci->intf = d;
    cxl_init_cci(cci, payload_max);
}

static const struct cxl_cmd cxl_cmd_set_t3_mctp[256][256] = {
    [INFOSTAT][IS_IDENTIFY] = { "IDENTIFY", cmd_infostat_identify, 0, 18 },
};

void cxl_initialize_t3_mctpcci(CXLCCI *cci, DeviceState *d, DeviceState *intf,
                               size_t payload_max)
{
    cxl_copy_cci_commands(cci, cxl_cmd_set_t3_mctp);
    cci->d = d;
    cci->intf = intf;
    cxl_init_cci(cci, payload_max);
}

static const struct cxl_cmd cxl_cmd_set_usp_mctp[256][256] = {
    [INFOSTAT][IS_IDENTIFY] = { "IDENTIFY", cmd_infostat_identify, 0, 18 },
    [PHYSICAL_SWITCH][IDENTIFY_SWITCH_DEVICE] = {"IDENTIFY_SWITCH_DEVICE",
        cmd_identify_switch_device, 0, 0x49 },
    [PHYSICAL_SWITCH][GET_PHYSICAL_PORT_STATE] = { "SWITCH_PHYSICAL_PORT_STATS",
        cmd_get_physical_port_state, ~0, ~0 },
};

void cxl_initialize_usp_mctpcci(CXLCCI *cci, DeviceState *d, DeviceState *intf,
                                size_t payload_max)
{
    cxl_copy_cci_commands(cci, cxl_cmd_set_usp_mctp);
    cci->d = d;
    cci->intf = intf;
    cxl_init_cci(cci, payload_max);
}
