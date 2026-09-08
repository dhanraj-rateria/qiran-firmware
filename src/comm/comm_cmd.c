#include "qiran/comm/comm_cmd.h"

#include "qiran/comm/comm_bytes.h"
#include "qiran/plat/plat_isr_uart.h"
#include "qiran/svc/svc_crc.h"
#include "qiran/svc/svc_fdir.h"
#include "qiran/svc/svc_log.h"
#include "qiran/svc/svc_time.h"

#define ST(s) ((uint32_t)1U << (uint32_t)(s))

#define ANY_STATE  (ST(SPR_COUNT) - 1U)
#define EARLY_ONLY (ST(SPR_BOOT) | ST(SPR_PRECOND))

static const comm_cmd_def_t k_cmd[COMM_CMD_COUNT] = {
    { "payload_on",        EARLY_ONLY, false, 0, 0,     2000U },
    { "processor_on",      EARLY_ONLY, false, 0, 0,     2000U },
    { "payload_off",       ANY_STATE,  false, 0, 0,     2000U },
    { "laser_on",          EARLY_ONLY, false, 0, 0,     5000U },
    { "laser_off",         ANY_STATE,  false, 0, 0,     1000U },
    { "heaters_on",        EARLY_ONLY, false, 0, 0,     2000U },
    { "heaters_off",       ANY_STATE,  false, 0, 0,     1000U },
    /*
     * Exactly one operating mode exists, so the only mode a mode command can
     * select is the one already running, and selecting it is a no-op that
     * succeeds. Any other selector value is out of range: that limit is derived
     * from there being one mode, not chosen.
     *
     * OPEN: what else a mode command is intended to do, since its effect is
     * left blank in the telecommand list.
     */
    { "mode",              ANY_STATE,  true,  0, 0,     1000U },
    { "processor_reset",   ANY_STATE,  false, 0, 0,     1000U },
    { "power_on_reset",    ANY_STATE,  false, 0, 0,     1000U },
    { "flight_mode_reset", ANY_STATE,  false, 0, 0,     1000U }
};

QIRAN_STATIC_ASSERT(QIRAN_ARRAY_LEN(k_cmd) == (size_t)COMM_CMD_COUNT,
                    every_command_is_defined);
QIRAN_STATIC_ASSERT(SPR_COUNT < 32, state_mask_covers_every_state);

static comm_cmd_handler_t s_handler[COMM_CMD_COUNT];
static void              *s_ctx[COMM_CMD_COUNT];

static comm_cmd_tx_fn_t s_tx;
static void            *s_tx_ctx;

static uint8_t  s_asm[COMM_CMD_FRAME_BYTES];
static uint32_t s_asm_len;

static struct {
    bool          active;
    comm_cmd_id_t id;
    uint32_t      parameter;
    uint16_t      sequence;
    uint32_t      receipt_ms;
    svc_timeout_t budget;
} s_inflight;

static uint32_t s_received;
static uint32_t s_completed;
static uint32_t s_rejected;
static uint32_t s_failed;
static uint32_t s_resyncs;
static uint32_t s_last_receipt_ms;

/* Selecting the only mode there is. */
static qiran_status_t mode_handler(void *ctx, uint32_t parameter)
{
    QIRAN_UNUSED(ctx);
    QIRAN_UNUSED(parameter);
    return QIRAN_OK;
}

void comm_cmd_init(void)
{
    uint32_t i;

    for (i = 0U; i < (uint32_t)COMM_CMD_COUNT; i++) {
        s_handler[i] = NULL;
        s_ctx[i] = NULL;
    }

    s_tx = NULL;
    s_tx_ctx = NULL;
    s_asm_len = 0U;
    s_inflight.active = false;
    s_received = 0U;
    s_completed = 0U;
    s_rejected = 0U;
    s_failed = 0U;
    s_resyncs = 0U;
    s_last_receipt_ms = 0U;

    /*
     * Owned here rather than registered, because it follows from there being a
     * single mode rather than from any hardware, and overriding it would mean
     * revisiting that.
     */
    s_handler[CMD_MODE] = mode_handler;
}

qiran_status_t comm_cmd_register(comm_cmd_id_t id, comm_cmd_handler_t handler,
                                 void *ctx)
{
    if ((id >= COMM_CMD_COUNT) || (handler == NULL)) {
        return QIRAN_ERR_PARAM;
    }
    if (s_handler[id] != NULL) {
        return QIRAN_ERR_STATE;
    }

    s_handler[id] = handler;
    s_ctx[id] = ctx;
    return QIRAN_OK;
}

qiran_status_t comm_cmd_set_transmit(comm_cmd_tx_fn_t transmit, void *ctx)
{
    s_tx = transmit;
    s_tx_ctx = ctx;
    return QIRAN_OK;
}

const comm_cmd_def_t *comm_cmd_def(comm_cmd_id_t id)
{
    return (id < COMM_CMD_COUNT) ? &k_cmd[id] : NULL;
}

bool comm_cmd_legal_in(comm_cmd_id_t id, mission_state_id_t state)
{
    if ((id >= COMM_CMD_COUNT) || (state >= SPR_COUNT)) {
        return false;
    }
    return (k_cmd[id].state_mask & ST(state)) != 0U;
}

const char *comm_cmd_result_name(comm_cmd_result_t result)
{
    switch (result) {
    case COMM_CMD_COMPLETED:     return "completed";
    case COMM_CMD_ACCEPTED:      return "accepted";
    case COMM_CMD_REJECT_FORMAT: return "format";
    case COMM_CMD_REJECT_UNKNOWN:return "unknown";
    case COMM_CMD_REJECT_STATE:  return "state";
    case COMM_CMD_REJECT_PARAM:  return "parameter";
    case COMM_CMD_REJECT_BUSY:   return "busy";
    case COMM_CMD_FAILED:        return "failed";
    case COMM_CMD_TIMEOUT:       return "timeout";
    default:                     return "?";
    }
}

qiran_status_t comm_cmd_build(comm_cmd_id_t id, uint32_t parameter,
                              uint16_t sequence, uint8_t *buf, uint32_t len,
                              uint32_t *written)
{
    uint32_t at = 0U;

    if ((buf == NULL) || (len < COMM_CMD_FRAME_BYTES)) {
        return QIRAN_ERR_PARAM;
    }

    at = comm_put_u8(buf, at, COMM_CMD_PACKET_ID);
    at = comm_put_u8(buf, at, COMM_CMD_VERSION);
    at = comm_put_u8(buf, at, (uint8_t)id);
    at = comm_put_u32(buf, at, parameter);
    at = comm_put_u16(buf, at, sequence);
    at = comm_put_u32(buf, at, svc_crc32(buf, at));

    if (written != NULL) {
        *written = at;
    }

    return (at == COMM_CMD_FRAME_BYTES) ? QIRAN_OK : QIRAN_ERR_INTEGRITY;
}

static void send_result(uint8_t id, uint16_t sequence, uint32_t receipt_ms,
                        comm_cmd_result_t result, uint32_t detail)
{
    uint8_t buf[COMM_CMD_RESULT_BYTES];
    uint32_t at = 0U;

    svc_log_event((uint16_t)(((uint32_t)id << 8) | (uint32_t)result), detail);

    if (s_tx == NULL) {
        return;
    }

    at = comm_put_u8(buf, at, COMM_CMD_RESULT_ID);
    at = comm_put_u8(buf, at, COMM_CMD_VERSION);
    at = comm_put_u32(buf, at, receipt_ms);
    at = comm_put_u16(buf, at, sequence);
    at = comm_put_u8(buf, at, id);
    at = comm_put_u8(buf, at, (uint8_t)result);
    at = comm_put_u32(buf, at, detail);
    at = comm_put_u32(buf, at, svc_crc32(buf, at));

    (void)s_tx(s_tx_ctx, buf, at);
}

static void reject(uint8_t id, uint16_t sequence, uint32_t receipt_ms,
                   comm_cmd_result_t result, uint32_t detail)
{
    s_rejected++;
    svc_fdir_report(QIRAN_FAULT_COMMAND_REJECTED, ((uint32_t)id << 8) |
                    (uint32_t)result);
    send_result(id, sequence, receipt_ms, result, detail);
}

static void dispatch(comm_cmd_id_t id, uint32_t parameter, uint16_t sequence,
                     uint32_t receipt_ms)
{
    qiran_status_t st;

    if (s_handler[id] == NULL) {
        /*
         * The command is well formed and legal, but nothing can carry it out.
         * That is a failure to execute rather than a rejection, and is reported
         * as such so it is not mistaken for the ground sending something wrong.
         */
        s_failed++;
        svc_fdir_report(QIRAN_FAULT_COMMAND_FAILED, (uint32_t)id);
        send_result((uint8_t)id, sequence, receipt_ms, COMM_CMD_FAILED,
                    (uint32_t)QIRAN_ERR_UNSUPPORTED);
        return;
    }

    st = s_handler[id](s_ctx[id], parameter);

    if (st == QIRAN_OK) {
        s_completed++;
        send_result((uint8_t)id, sequence, receipt_ms, COMM_CMD_COMPLETED, 0U);
        return;
    }

    if (st == QIRAN_ERR_BUSY) {
        s_inflight.active = true;
        s_inflight.id = id;
        s_inflight.parameter = parameter;
        s_inflight.sequence = sequence;
        s_inflight.receipt_ms = receipt_ms;
        svc_timeout_arm(&s_inflight.budget, k_cmd[id].budget_ms);
        send_result((uint8_t)id, sequence, receipt_ms, COMM_CMD_ACCEPTED, 0U);
        return;
    }

    s_failed++;
    svc_fdir_report(QIRAN_FAULT_COMMAND_FAILED, (uint32_t)id);
    send_result((uint8_t)id, sequence, receipt_ms, COMM_CMD_FAILED,
                (uint32_t)st);
}

/* The sequence: format, decode, legality, parameter, dispatch. */
static void process(const uint8_t *frame)
{
    uint8_t raw_id = frame[2];
    uint32_t parameter = comm_get_u32(frame, 3);
    uint16_t sequence = comm_get_u16(frame, 7);
    uint32_t receipt_ms = (uint32_t)svc_time_now_ms();
    comm_cmd_id_t id;

    s_received++;
    s_last_receipt_ms = receipt_ms;

    if (frame[1] != COMM_CMD_VERSION) {
        reject(raw_id, sequence, receipt_ms, COMM_CMD_REJECT_FORMAT, frame[1]);
        return;
    }

    if (raw_id >= (uint8_t)COMM_CMD_COUNT) {
        reject(raw_id, sequence, receipt_ms, COMM_CMD_REJECT_UNKNOWN, raw_id);
        return;
    }

    id = (comm_cmd_id_t)raw_id;

    /* One at a time, so a result always refers to an unambiguous command. */
    if (s_inflight.active) {
        reject(raw_id, sequence, receipt_ms, COMM_CMD_REJECT_BUSY,
               (uint32_t)s_inflight.id);
        return;
    }

    if (!comm_cmd_legal_in(id, mission_state_current())) {
        reject(raw_id, sequence, receipt_ms, COMM_CMD_REJECT_STATE,
               (uint32_t)mission_state_current());
        return;
    }

    if (k_cmd[id].takes_parameter) {
        int32_t value = (int32_t)parameter;

        if ((value < k_cmd[id].param_min) || (value > k_cmd[id].param_max)) {
            reject(raw_id, sequence, receipt_ms, COMM_CMD_REJECT_PARAM,
                   parameter);
            return;
        }
    }

    dispatch(id, parameter, sequence, receipt_ms);
}

qiran_status_t comm_cmd_submit(const uint8_t *frame, uint32_t len)
{
    uint32_t stored;

    if ((frame == NULL) || (len != COMM_CMD_FRAME_BYTES)) {
        return QIRAN_ERR_PARAM;
    }
    if (frame[0] != COMM_CMD_PACKET_ID) {
        return QIRAN_ERR_PARAM;
    }

    stored = comm_get_u32(frame, COMM_CMD_FRAME_BYTES - 4U);
    if (stored != svc_crc32(frame, COMM_CMD_FRAME_BYTES - 4U)) {
        return QIRAN_ERR_INTEGRITY;
    }

    process(frame);
    return QIRAN_OK;
}

/*
 * Drops the leading byte, then any further bytes up to the next start marker,
 * so a frame corrupted or truncated on the link cannot desynchronise the
 * stream permanently.
 */
static void resync(void)
{
    uint32_t i;
    uint32_t keep = 0U;

    s_resyncs++;

    for (i = 1U; i < s_asm_len; i++) {
        if (s_asm[i] == COMM_CMD_PACKET_ID) {
            keep = s_asm_len - i;
            {
                uint32_t j;
                for (j = 0U; j < keep; j++) {
                    s_asm[j] = s_asm[i + j];
                }
            }
            break;
        }
    }

    s_asm_len = keep;
}

static void assemble(void)
{
    uint8_t chunk[64];
    uint32_t n;

    /*
     * Bounded by the receive ring's own size, so the work done here cannot grow
     * with how much has arrived beyond what the ring could hold.
     */
    do {
        uint32_t i;

        n = plat_isr_uart_read(chunk, (uint32_t)sizeof(chunk));

        for (i = 0U; i < n; i++) {
            if ((s_asm_len == 0U) && (chunk[i] != COMM_CMD_PACKET_ID)) {
                continue;   /* not a frame start; wait for one */
            }

            s_asm[s_asm_len] = chunk[i];
            s_asm_len++;

            if (s_asm_len == COMM_CMD_FRAME_BYTES) {
                uint32_t stored = comm_get_u32(s_asm, COMM_CMD_FRAME_BYTES - 4U);

                if (stored == svc_crc32(s_asm, COMM_CMD_FRAME_BYTES - 4U)) {
                    process(s_asm);
                    s_asm_len = 0U;
                } else {
                    resync();
                }
            }
        }
    } while (n == sizeof(chunk));
}

static void track(void)
{
    qiran_status_t st;

    if (!s_inflight.active) {
        return;
    }

    st = s_handler[s_inflight.id](s_ctx[s_inflight.id], s_inflight.parameter);

    if (st == QIRAN_ERR_BUSY) {
        if (svc_timeout_expired(&s_inflight.budget)) {
            s_inflight.active = false;
            s_failed++;
            svc_fdir_report(QIRAN_FAULT_COMMAND_FAILED,
                            (uint32_t)s_inflight.id);
            send_result((uint8_t)s_inflight.id, s_inflight.sequence,
                        s_inflight.receipt_ms, COMM_CMD_TIMEOUT,
                        k_cmd[s_inflight.id].budget_ms);
        }
        return;
    }

    s_inflight.active = false;

    if (st == QIRAN_OK) {
        s_completed++;
        send_result((uint8_t)s_inflight.id, s_inflight.sequence,
                    s_inflight.receipt_ms, COMM_CMD_COMPLETED, 0U);
    } else {
        s_failed++;
        svc_fdir_report(QIRAN_FAULT_COMMAND_FAILED, (uint32_t)s_inflight.id);
        send_result((uint8_t)s_inflight.id, s_inflight.sequence,
                    s_inflight.receipt_ms, COMM_CMD_FAILED, (uint32_t)st);
    }
}

void comm_cmd_service(void)
{
    plat_irq_event_t ev;

    /*
     * An idle gap on the link marks a frame boundary, so a partially received
     * frame is abandoned rather than being completed by the bytes of the next
     * one.
     */
    while (plat_isr_uart_take(&ev)) {
        if ((ev.kind == PLAT_UART_KIND_GAP) && (s_asm_len != 0U)) {
            s_asm_len = 0U;
            s_resyncs++;
        }
    }

    track();
    assemble();
}

uint32_t comm_cmd_received(void)        { return s_received; }
uint32_t comm_cmd_completed(void)       { return s_completed; }
uint32_t comm_cmd_rejected(void)        { return s_rejected; }
uint32_t comm_cmd_failed(void)          { return s_failed; }
uint32_t comm_cmd_resyncs(void)         { return s_resyncs; }
bool     comm_cmd_in_progress(void)     { return s_inflight.active; }
uint32_t comm_cmd_last_receipt_ms(void) { return s_last_receipt_ms; }
