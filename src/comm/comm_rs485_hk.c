#include "qiran/comm/comm_rs485_hk.h"

#include "qiran/comm/comm_bytes.h"
#include "qiran/mission/mission_seq.h"
#include "qiran/qiran_config.h"
#include "qiran/svc/svc_config.h"
#include "qiran/svc/svc_crc.h"
#include "qiran/svc/svc_fdir.h"
#include "qiran/svc/svc_health.h"
#include "qiran/svc/svc_log.h"
#include "qiran/svc/svc_time.h"
#include "qiran/svc/svc_watchdog.h"

static comm_hk_sample_fn_t s_sample;
static void               *s_sample_ctx;
static comm_hk_tx_fn_t     s_tx;
static void               *s_tx_ctx;

static comm_interlock_status_t s_interlock[COMM_HK_INTERLOCKS];

static uint32_t s_frames;
static uint32_t s_status_frames;
static uint32_t s_tx_failures;
static uint32_t s_sample_failures;
static uint64_t s_last_attempt_ms;
static bool     s_attempted;

static uint8_t  s_frame[COMM_HK_FRAME_BYTES];
static uint32_t s_staged;
static uint8_t s_status[COMM_HK_STATUS_BYTES];

void comm_rs485_hk_init(void)
{
    uint32_t i;

    s_sample = NULL;
    s_sample_ctx = NULL;
    s_tx = NULL;
    s_tx_ctx = NULL;
    s_frames = 0U;
    s_status_frames = 0U;
    s_tx_failures = 0U;
    s_sample_failures = 0U;
    s_last_attempt_ms = 0U;
    s_attempted = false;
    s_staged = 0U;

    for (i = 0U; i < COMM_HK_INTERLOCKS; i++) {
        s_interlock[i] = COMM_INTERLOCK_UNEVALUATED;
    }
}

qiran_status_t comm_rs485_hk_set_sampler(comm_hk_sample_fn_t sample, void *ctx)
{
    s_sample = sample;
    s_sample_ctx = ctx;
    return QIRAN_OK;
}

qiran_status_t comm_rs485_hk_set_transmit(comm_hk_tx_fn_t transmit, void *ctx)
{
    s_tx = transmit;
    s_tx_ctx = ctx;
    return QIRAN_OK;
}

void comm_rs485_hk_set_interlock(uint32_t index, comm_interlock_status_t status)
{
    if (index < COMM_HK_INTERLOCKS) {
        s_interlock[index] = status;
    }
}

static void zero_sample(comm_hk_sample_t *s)
{
    uint32_t i;

    for (i = 0U; i < COMM_HK_RAILS; i++) {
        s->rail_mv[i] = 0U;
    }
    for (i = 0U; i < COMM_HK_BOARD_SENSORS; i++) {
        s->board_temp_c100[i] = 0;
    }
    for (i = 0U; i < COMM_HK_PL_ERROR_BYTES; i++) {
        s->pl_error[i] = 0U;
    }

    s->ps_temp_c100 = 0;
    s->ddr_temp_c100 = 0;
    s->ps_clock_hz = 0U;
    s->pl_clock_hz = 0U;
    s->por_status = 0U;
    s->pl_config_done = 0U;
    s->overcurrent_flags = 0U;
    s->brownout_flags = 0U;
    s->boot_mode = 0U;
}

qiran_status_t comm_rs485_hk_build(uint8_t *buf, uint32_t len, uint32_t *written)
{
    comm_hk_sample_t sample;
    uint32_t at = 0U;
    uint32_t i;
    bool sampled = false;

    if ((buf == NULL) || (len < COMM_HK_FRAME_BYTES)) {
        return QIRAN_ERR_PARAM;
    }

    /*
     * A frame goes out whether or not the board can be read. Its arrival is
     * what proves to the observer that the payload is still executing, so
     * withholding it because a sensor is unavailable would trade a missing
     * reading for an apparent loss of the payload.
     */
    zero_sample(&sample);

    if (s_sample != NULL) {
        if (s_sample(s_sample_ctx, &sample) == QIRAN_OK) {
            sampled = true;
        } else {
            zero_sample(&sample);
        }
    }

    if (!sampled) {
        s_sample_failures++;
        svc_fdir_report(QIRAN_FAULT_HK_SAMPLE, (s_sample == NULL) ? 0U : 1U);
    }

    at = comm_put_u8(buf, at, COMM_HK_PACKET_ID);
    at = comm_put_u8(buf, at, COMM_HK_PACKET_VERSION);
    at = comm_put_u32(buf, at, (uint32_t)svc_time_now_ms());

    for (i = 0U; i < COMM_HK_RAILS; i++) {
        at = comm_put_u16(buf, at, sample.rail_mv[i]);
    }

    at = comm_put_i16(buf, at, sample.ps_temp_c100);
    for (i = 0U; i < COMM_HK_BOARD_SENSORS; i++) {
        at = comm_put_i16(buf, at, sample.board_temp_c100[i]);
    }
    at = comm_put_i16(buf, at, sample.ddr_temp_c100);

    at = comm_put_u32(buf, at, sample.ps_clock_hz);
    at = comm_put_u32(buf, at, sample.pl_clock_hz);

    at = comm_put_u8(buf, at, sample.por_status);
    at = comm_put_u8(buf, at, sample.pl_config_done);
    at = comm_put_u8(buf, at, svc_watchdog_armed() ? 1U : 0U);
    at = comm_put_u8(buf, at, sample.overcurrent_flags);
    at = comm_put_u8(buf, at, sample.brownout_flags);

    for (i = 0U; i < COMM_HK_PL_ERROR_BYTES; i++) {
        at = comm_put_u8(buf, at, sample.pl_error[i]);
    }

    at = comm_put_u8(buf, at, sample.boot_mode);
    at = comm_put_u32(buf, at, (uint32_t)(svc_time_now_ms() / 1000U));

    {
        uint32_t faults = svc_log_fault_total();

        at = comm_put_u16(buf, at,
                          (faults > 0xFFFFU) ? 0xFFFFU : (uint16_t)faults);
    }

    at = comm_put_u32(buf, at, svc_crc32(buf, at));

    if (written != NULL) {
        *written = at;
    }

    return (at == COMM_HK_FRAME_BYTES) ? QIRAN_OK : QIRAN_ERR_INTEGRITY;
}

qiran_status_t comm_rs485_status_build(const comm_status_report_t *report,
                                       uint8_t *buf, uint32_t len,
                                       uint32_t *written)
{
    uint32_t at = 0U;
    uint32_t i;

    if ((report == NULL) || (buf == NULL) || (len < COMM_HK_STATUS_BYTES)) {
        return QIRAN_ERR_PARAM;
    }

    at = comm_put_u8(buf, at, COMM_HK_STATUS_ID);
    at = comm_put_u8(buf, at, COMM_HK_PACKET_VERSION);
    at = comm_put_u32(buf, at, (uint32_t)svc_time_now_ms());

    at = comm_put_u8(buf, at, (uint8_t)report->payload);
    at = comm_put_u8(buf, at, (uint8_t)report->health);

    for (i = 0U; i < COMM_HK_INTERLOCKS; i++) {
        at = comm_put_u8(buf, at, (uint8_t)report->interlock[i]);
    }

    at = comm_put_u16(buf, at, (uint16_t)report->fault);
    at = comm_put_u8(buf, at, (uint8_t)report->severity);

    at = comm_put_u32(buf, at, svc_crc32(buf, at));

    if (written != NULL) {
        *written = at;
    }

    return (at == COMM_HK_STATUS_BYTES) ? QIRAN_OK : QIRAN_ERR_INTEGRITY;
}

static qiran_status_t transmit(const uint8_t *data, uint32_t len)
{
    qiran_status_t st;

    if (s_tx == NULL) {
        s_tx_failures++;
        svc_fdir_report(QIRAN_FAULT_HK_TRANSMIT, 0U);
        return QIRAN_ERR_UNSUPPORTED;
    }

    st = s_tx(s_tx_ctx, data, len);
    if (st != QIRAN_OK) {
        s_tx_failures++;
        svc_fdir_report(QIRAN_FAULT_HK_TRANSMIT, (uint32_t)st);
    }

    return st;
}

void comm_rs485_hk_stage(void)
{
    uint64_t now = svc_time_now_ms();
    uint32_t period = (uint32_t)svc_config_get(CFG_HK_PERIOD_MS);
    uint32_t written = 0U;

    /*
     * Elapsed since the last attempt rather than a deadline fixed when that
     * attempt was made, so a change to the interval takes effect at once
     * instead of after the interval it replaced.
     */
    if (s_attempted && ((now - s_last_attempt_ms) < (uint64_t)period)) {
        return;
    }

    /*
     * Marked before building, so a link that keeps refusing is retried on the
     * interval rather than on every cycle.
     */
    s_last_attempt_ms = now;
    s_attempted = true;

    if (comm_rs485_hk_build(s_frame, sizeof(s_frame), &written) == QIRAN_OK) {
        s_staged = written;
    }
}

void comm_rs485_hk_transmit(void)
{
    if (s_staged == 0U) {
        return;
    }

    if (transmit(s_frame, s_staged) == QIRAN_OK) {
        s_frames++;
    }

    /*
     * Dropped whether or not it went. The next frame carries current readings,
     * and holding a stale one back would delay those to re-send measurements
     * already superseded.
     */
    s_staged = 0U;
}

qiran_status_t comm_rs485_status_send(const comm_status_report_t *report)
{
    uint32_t written = 0U;
    qiran_status_t st;

    st = comm_rs485_status_build(report, s_status, sizeof(s_status), &written);
    if (st != QIRAN_OK) {
        return st;
    }

    st = transmit(s_status, written);
    if (st == QIRAN_OK) {
        s_status_frames++;
    }

    return st;
}

/* --- delivery of faults over this link --- */

static void fill_current(comm_status_report_t *r)
{
    uint32_t i;

    r->payload = mission_payload_status();
    r->health = svc_health_state();
    for (i = 0U; i < COMM_HK_INTERLOCKS; i++) {
        r->interlock[i] = s_interlock[i];
    }
}

static qiran_status_t uplink_report(qiran_fault_id_t id,
                                    qiran_severity_t severity,
                                    uint32_t detail)
{
    comm_status_report_t r;

    QIRAN_UNUSED(detail);

    fill_current(&r);
    r.fault = id;
    r.severity = severity;

    return comm_rs485_status_send(&r);
}

/*
 * A power cycle is requested by reporting the fault that caused it at the
 * severe tier. There is no separate request frame, because the frame definition
 * offers none and inventing one would put an unreviewed packet on the link.
 *
 * OPEN: confirm with the interface owner how a power-cycle request is
 * distinguished from a severe fault report.
 */
static qiran_status_t uplink_power_reset(qiran_fault_id_t id)
{
    comm_status_report_t r;

    fill_current(&r);
    r.fault = id;
    r.severity = QIRAN_SEV_SEVERE;

    return comm_rs485_status_send(&r);
}

static const svc_fdir_uplink_t k_uplink = { uplink_report, uplink_power_reset };

const svc_fdir_uplink_t *comm_rs485_hk_uplink(void)
{
    return &k_uplink;
}

uint32_t comm_rs485_hk_frames_sent(void)        { return s_frames; }
uint32_t comm_rs485_hk_status_sent(void)        { return s_status_frames; }
uint32_t comm_rs485_hk_transmit_failures(void)  { return s_tx_failures; }
uint32_t comm_rs485_hk_sample_failures(void)    { return s_sample_failures; }
bool     comm_rs485_hk_ready(void)              { return s_tx != NULL; }
