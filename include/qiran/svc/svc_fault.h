#ifndef QIRAN_SVC_FAULT_H
#define QIRAN_SVC_FAULT_H

#include "qiran/qiran_types.h"

/*
 * The four severity tiers. There is no fifth tier for the case where sequential
 * execution is not guaranteed: that case cannot be reported by software, since
 * software reporting it would prove it still runs. It is detected by the
 * watchdog gate and by the observer's absence-of-communication timeout, and so
 * appears here only as the consequence of a missed watchdog service.
 */
typedef enum {
    QIRAN_SEV_MINOR = 0,
    QIRAN_SEV_MEDIUM,
    QIRAN_SEV_CRITICAL,
    QIRAN_SEV_SEVERE,
    QIRAN_SEV_COUNT
} qiran_severity_t;

typedef enum {
    QIRAN_ESCALATE_LOG_ONLY = 0,
    QIRAN_ESCALATE_REPORT,
    QIRAN_ESCALATE_POWER_RESET
} qiran_escalation_t;

/*
 * Every fault condition in the system, so that reporting is one common path and
 * no module carries a private error code that bypasses it.
 *
 * Tier and escalation for each are in the policy table in svc_fdir.c. Where the
 * requirements do not state a tier explicitly, the assignment there is
 * provisional and that table is the single place to review it.
 */
typedef enum {
    QIRAN_FAULT_NONE = 0,

    QIRAN_FAULT_BOOT_IMAGE,
    QIRAN_FAULT_SAFE_OUTPUTS,
    QIRAN_FAULT_POST,
    QIRAN_FAULT_DEVINIT,
    QIRAN_FAULT_LINK_ESTABLISH,
    QIRAN_FAULT_CYCLE_OVERRUN,
    QIRAN_FAULT_IRQ_OVERFLOW,
    QIRAN_FAULT_UART_RX_OVERFLOW,
    QIRAN_FAULT_PL_ERROR,
    QIRAN_FAULT_WATCHDOG_EXPIRY,
    QIRAN_FAULT_MEMORY,
    QIRAN_FAULT_CONFIG,
    QIRAN_FAULT_STATE_TRANSITION,
    QIRAN_FAULT_STATE_TIMEOUT,
    QIRAN_FAULT_PRECOND,
    QIRAN_FAULT_CONTROL_LOOP,

    QIRAN_FAULT_OVERCURRENT,
    QIRAN_FAULT_BROWNOUT,
    QIRAN_FAULT_TEMPERATURE,

    QIRAN_FAULT_LASER_TEC,
    QIRAN_FAULT_LASER_POWER,
    QIRAN_FAULT_MRR_LOCK,
    QIRAN_FAULT_CROW_TUNE,
    QIRAN_FAULT_UMZI_TUNE,
    QIRAN_FAULT_DLI_LOCK,
    QIRAN_FAULT_MRR_RELOCK,

    QIRAN_FAULT_SPAD_OPTICAL_POWER,
    QIRAN_FAULT_SPAD_DARK_COUNT,

    QIRAN_FAULT_SPW_LINK,
    QIRAN_FAULT_PACKET_INTEGRITY,
    QIRAN_FAULT_PACKET_SEQUENCE,
    QIRAN_FAULT_OUTPUT_QUEUE,
    QIRAN_FAULT_HK_SAMPLE,
    QIRAN_FAULT_HK_TRANSMIT,
    QIRAN_FAULT_COMMAND_REJECTED,
    QIRAN_FAULT_COMMAND_FAILED,

    QIRAN_FAULT_DDR_OVERRUN,
    QIRAN_FAULT_NAND_WRITE,
    QIRAN_FAULT_BUFFER_OWNERSHIP,
    QIRAN_FAULT_DATA_PRODUCT,

    QIRAN_FAULT_COUNT
} qiran_fault_id_t;

#endif
