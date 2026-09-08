#ifndef QIRAN_COMM_CMD_H
#define QIRAN_COMM_CMD_H

#include "qiran/mission/mission_state.h"
#include "qiran/qiran_types.h"

/*
 * The telecommand set. Their effects are hardware actions, so each is carried
 * out by a handler registered by whoever owns that hardware; this module owns
 * the receiving, checking, dispatch, tracking and result reporting around them.
 */
typedef enum {
    CMD_PAYLOAD_ON = 0,      /* both supplies on */
    CMD_PROCESSOR_ON,        /* processor supply only */
    CMD_PAYLOAD_OFF,         /* both supplies off */
    CMD_LASER_ON,
    CMD_LASER_OFF,
    CMD_HEATERS_ON,
    CMD_HEATERS_OFF,
    CMD_MODE,
    CMD_PROCESSOR_RESET,
    CMD_POWER_ON_RESET,
    CMD_FLIGHT_MODE_RESET,
    COMM_CMD_COUNT
} comm_cmd_id_t;

/*
 * Outcomes reported back for every command, accepted or not. The three
 * rejection reasons match the three checks the command sequence makes before
 * dispatch, so a rejection says which check refused it.
 */
typedef enum {
    COMM_CMD_COMPLETED = 0,
    COMM_CMD_ACCEPTED,             /* dispatched, still running */
    COMM_CMD_REJECT_FORMAT = 16,
    COMM_CMD_REJECT_UNKNOWN,
    COMM_CMD_REJECT_STATE,
    COMM_CMD_REJECT_PARAM,
    COMM_CMD_REJECT_BUSY,
    COMM_CMD_FAILED = 32,
    COMM_CMD_TIMEOUT
} comm_cmd_result_t;

/*
 * Carries out the command. Returning QIRAN_OK completes it; QIRAN_ERR_BUSY
 * leaves it running and the handler is called again on later cycles until it
 * finishes or its budget expires; anything else fails it.
 *
 * Called from the cyclic loop and subject to the same no-blocking rule as every
 * other task.
 */
typedef qiran_status_t (*comm_cmd_handler_t)(void *ctx, uint32_t parameter);

typedef qiran_status_t (*comm_cmd_tx_fn_t)(void *ctx, const uint8_t *data,
                                           uint32_t len);

#define COMM_CMD_PACKET_ID     0xC5U
#define COMM_CMD_RESULT_ID     0xC6U
#define COMM_CMD_VERSION       1U

/*
 * OPEN: no command frame layout is given in the source documents. This one
 * mirrors the housekeeping frames so both use the same header shape and the
 * same checksum, and is marked provisional.
 */
#define COMM_CMD_FRAME_BYTES   13U
#define COMM_CMD_RESULT_BYTES  18U

typedef struct {
    const char *name;
    /*
     * States the command may be given in, one bit per state.
     *
     * OPEN: which commands may override a stage already in progress is an
     * unanswered design question. Pending an answer these masks follow a
     * conservative rule that is stated rather than assumed: a command that
     * removes energy from the payload is accepted in any state, because
     * refusing to turn something off is never the safer choice; a command that
     * applies energy is accepted only before the sequence has committed to an
     * optical configuration, because applying it later would contradict the
     * stage that owns that hardware; and the reset commands are accepted in any
     * state, because recovery has to work when things are already wrong.
     */
    uint32_t state_mask;
    bool     takes_parameter;
    int32_t  param_min;
    int32_t  param_max;
    uint32_t budget_ms;
} comm_cmd_def_t;

void comm_cmd_init(void);

qiran_status_t comm_cmd_register(comm_cmd_id_t id, comm_cmd_handler_t handler,
                                 void *ctx);
qiran_status_t comm_cmd_set_transmit(comm_cmd_tx_fn_t transmit, void *ctx);

/* Assembles frames from the receive ring, then runs the command sequence. */
void comm_cmd_service(void);

/*
 * Runs the command sequence on a frame that has already been delimited, which
 * is how a command arriving inside a space packet is handled. Both links share
 * one sequence rather than each having its own.
 */
qiran_status_t comm_cmd_submit(const uint8_t *frame, uint32_t len);

const comm_cmd_def_t *comm_cmd_def(comm_cmd_id_t id);
const char           *comm_cmd_result_name(comm_cmd_result_t result);
bool                  comm_cmd_legal_in(comm_cmd_id_t id, mission_state_id_t state);

/* Builds a command frame. Present so a bench sender and the tests agree. */
qiran_status_t comm_cmd_build(comm_cmd_id_t id, uint32_t parameter,
                              uint16_t sequence, uint8_t *buf, uint32_t len,
                              uint32_t *written);

uint32_t comm_cmd_received(void);
uint32_t comm_cmd_completed(void);
uint32_t comm_cmd_rejected(void);
uint32_t comm_cmd_failed(void);
uint32_t comm_cmd_resyncs(void);
bool     comm_cmd_in_progress(void);

/* Receipt time of the most recent accepted command, at minor-cycle resolution. */
uint32_t comm_cmd_last_receipt_ms(void);

#endif
