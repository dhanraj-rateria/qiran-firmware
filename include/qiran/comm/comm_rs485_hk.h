#ifndef QIRAN_COMM_RS485_HK_H
#define QIRAN_COMM_RS485_HK_H

#include "qiran/qiran_types.h"
#include "qiran/svc/svc_fault.h"
#include "qiran/svc/svc_fdir.h"

#define COMM_HK_PACKET_ID       0xA5U
#define COMM_HK_STATUS_ID       0xA6U
#define COMM_HK_PACKET_VERSION  1U

/* Byte counts from the frame definition, most significant byte first. */
#define COMM_HK_FRAME_BYTES    56U
#define COMM_HK_STATUS_BYTES   20U

#define COMM_HK_RAILS          5U   /* 1V0, 1V5, 1V8, 3V3, 5V */
#define COMM_HK_BOARD_SENSORS  2U
#define COMM_HK_INTERLOCKS     5U
#define COMM_HK_PL_ERROR_BYTES 8U

/*
 * The part of the frame that can only come from the board: rail voltages,
 * temperatures, measured clocks and the raw status bytes. Supplied by whoever
 * can reach those devices. The rest of the frame the flight software knows for
 * itself and fills in either way.
 */
typedef struct {
    uint16_t rail_mv[COMM_HK_RAILS];
    int16_t  ps_temp_c100;
    int16_t  board_temp_c100[COMM_HK_BOARD_SENSORS];
    int16_t  ddr_temp_c100;
    uint32_t ps_clock_hz;
    uint32_t pl_clock_hz;
    uint8_t  por_status;
    uint8_t  pl_config_done;
    uint8_t  overcurrent_flags;
    uint8_t  brownout_flags;
    uint8_t  pl_error[COMM_HK_PL_ERROR_BYTES];
    uint8_t  boot_mode;
} comm_hk_sample_t;

typedef qiran_status_t (*comm_hk_sample_fn_t)(void *ctx, comm_hk_sample_t *out);
typedef qiran_status_t (*comm_hk_tx_fn_t)(void *ctx, const uint8_t *data, uint32_t len);

/*
 * Status reported as formal parameters rather than as a bit pattern, since
 * which bit means what is an interface decision and not one to take here.
 *
 * The interlocks carry a third value for not yet evaluated. Before the detector
 * enable stage runs they have genuinely not been tested, and reporting either
 * pass or fail for an untested interlock would be a false statement.
 */
typedef enum {
    COMM_INTERLOCK_UNEVALUATED = 0,
    COMM_INTERLOCK_PASS,
    COMM_INTERLOCK_FAIL
} comm_interlock_status_t;

typedef struct {
    qiran_payload_status_t  payload;
    qiran_health_t          health;
    comm_interlock_status_t interlock[COMM_HK_INTERLOCKS];
    qiran_fault_id_t        fault;
    qiran_severity_t        severity;
} comm_status_report_t;

void comm_rs485_hk_init(void);

qiran_status_t comm_rs485_hk_set_sampler(comm_hk_sample_fn_t sample, void *ctx);
qiran_status_t comm_rs485_hk_set_transmit(comm_hk_tx_fn_t transmit, void *ctx);

/*
 * Builds the housekeeping frame into the caller's buffer. Exposed separately
 * from sending it so the exact bytes can be checked against the frame
 * definition without a link.
 */
qiran_status_t comm_rs485_hk_build(uint8_t *buf, uint32_t len, uint32_t *written);
qiran_status_t comm_rs485_status_build(const comm_status_report_t *report,
                                       uint8_t *buf, uint32_t len,
                                       uint32_t *written);

/* Sends the housekeeping frame on its configured interval. */
void comm_rs485_hk_service(void);

/* Sends a status report immediately, outside the periodic interval. */
qiran_status_t comm_rs485_status_send(const comm_status_report_t *report);

/*
 * Delivery of a fault to the observer, and the request for a power cycle, over
 * this link. Pass the result to the fault service so its escalations are
 * actually transmitted rather than only counted.
 */
const svc_fdir_uplink_t *comm_rs485_hk_uplink(void);

void comm_rs485_hk_set_interlock(uint32_t index, comm_interlock_status_t status);

uint32_t comm_rs485_hk_frames_sent(void);
uint32_t comm_rs485_hk_status_sent(void);
uint32_t comm_rs485_hk_transmit_failures(void);
uint32_t comm_rs485_hk_sample_failures(void);
bool     comm_rs485_hk_ready(void);

#endif
