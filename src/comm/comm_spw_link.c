#include "qiran/comm/comm_spw_link.h"

#include "qiran/comm/comm_ccsds.h"
#include "qiran/plat/plat_irq.h"
#include "qiran/svc/svc_config.h"
#include "qiran/svc/svc_fdir.h"
#include "qiran/svc/svc_log.h"
#include "qiran/svc/svc_time.h"

/*
 * Largest packet this module will assemble from the link in one go. Science
 * data is moved as descriptors rather than copied through here, so this bounds
 * command and control traffic only.
 */
#define SPW_RX_BYTES 512U

static comm_spw_ops_t   s_ops;
static bool             s_ops_set;
static comm_spw_state_t s_state;

static svc_timeout_t s_start_budget;
static uint32_t      s_attempts;
static uint32_t      s_disconnects;
static uint32_t      s_errors;
static uint32_t      s_sent;
static uint32_t      s_received;
static uint32_t      s_status_word;
static uint32_t      s_error_word;

static uint8_t s_rx[SPW_RX_BYTES];

static const char *const k_state_name[] = {
    "down", "starting", "run", "failed"
};

void comm_spw_link_init(void)
{
    s_ops.start = NULL;
    s_ops.status = NULL;
    s_ops.send = NULL;
    s_ops.receive = NULL;
    s_ops.ctx = NULL;
    s_ops_set = false;

    s_state = SPW_LINK_DOWN;
    s_attempts = 0U;
    s_disconnects = 0U;
    s_errors = 0U;
    s_sent = 0U;
    s_received = 0U;
    s_status_word = 0U;
    s_error_word = 0U;
    svc_timeout_disarm(&s_start_budget);
}

qiran_status_t comm_spw_link_set_ops(const comm_spw_ops_t *ops)
{
    if (ops == NULL) {
        comm_spw_link_init();
        return QIRAN_OK;
    }

    if ((ops->start == NULL) || (ops->status == NULL) ||
        (ops->send == NULL) || (ops->receive == NULL)) {
        return QIRAN_ERR_PARAM;
    }

    s_ops = *ops;
    s_ops_set = true;
    s_state = SPW_LINK_DOWN;
    return QIRAN_OK;
}

comm_spw_state_t comm_spw_link_state(void)
{
    return s_state;
}

const char *comm_spw_link_state_name(comm_spw_state_t state)
{
    return (state <= SPW_LINK_FAILED) ? k_state_name[state] : "?";
}

bool comm_spw_link_running(void)
{
    return s_state == SPW_LINK_RUN;
}

qiran_status_t comm_spw_link_send(const uint8_t *data, uint32_t len)
{
    qiran_status_t st;

    if ((data == NULL) || (len == 0U)) {
        return QIRAN_ERR_PARAM;
    }
    if (s_state != SPW_LINK_RUN) {
        return QIRAN_ERR_STATE;
    }

    st = s_ops.send(s_ops.ctx, data, len);
    if (st == QIRAN_OK) {
        s_sent++;
    } else {
        s_errors++;
        svc_fdir_report(QIRAN_FAULT_SPW_LINK, (uint32_t)st);
    }

    return st;
}

/*
 * Link errors are published by the interrupt and drained here, so a burst of
 * them costs one bounded pass through a queue rather than growing the
 * interrupt's own work.
 */
static void drain_link_events(void)
{
    plat_irq_event_t ev;

    while (plat_irq_take(PLAT_IRQ_SPW, &ev)) {
        s_errors++;
        s_error_word = ev.datum;
        svc_fdir_report(QIRAN_FAULT_SPW_LINK, ev.datum);
    }
}

static void deliver_received(void)
{
    uint32_t n;

    do {
        n = s_ops.receive(s_ops.ctx, s_rx, (uint32_t)sizeof(s_rx));

        if (n != 0U) {
            s_received++;
            /*
             * Handed on whole. A short or corrupted packet is rejected by the
             * packet layer's own length and checksum checks rather than being
             * second-guessed here.
             */
            (void)comm_ccsds_receive(s_rx, n);
        }
    } while (n == sizeof(s_rx));
}

void comm_spw_link_service(void)
{
    bool running = false;

    if (!s_ops_set) {
        return;
    }

    drain_link_events();

    switch (s_state) {
    case SPW_LINK_DOWN:
        s_attempts++;
        if (s_ops.start(s_ops.ctx) != QIRAN_OK) {
            s_errors++;
            svc_fdir_report(QIRAN_FAULT_SPW_LINK, 0U);
            /*
             * Left down rather than marked failed: the attempt counter is the
             * Critical-tier counter for this class, and it decides when to stop
             * trying.
             */
            if (svc_fdir_retries_exhausted(QIRAN_FAULT_SPW_LINK)) {
                s_state = SPW_LINK_FAILED;
            }
            return;
        }
        svc_timeout_arm(&s_start_budget,
                        (uint32_t)svc_config_get(CFG_SPW_START_TIMEOUT_MS));
        s_state = SPW_LINK_STARTING;
        break;

    case SPW_LINK_STARTING:
        if (s_ops.status(s_ops.ctx, &running, &s_status_word) != QIRAN_OK) {
            s_errors++;
            svc_fdir_report(QIRAN_FAULT_SPW_LINK, 0U);
            s_state = SPW_LINK_DOWN;
            return;
        }

        if (running) {
            s_state = SPW_LINK_RUN;
            svc_timeout_disarm(&s_start_budget);
            svc_log_event(0U, s_status_word);
        } else if (svc_timeout_expired(&s_start_budget)) {
            s_error_word = s_status_word;
            svc_fdir_report(QIRAN_FAULT_SPW_LINK, s_status_word);
            s_state = svc_fdir_retries_exhausted(QIRAN_FAULT_SPW_LINK)
                      ? SPW_LINK_FAILED : SPW_LINK_DOWN;
        }
        break;

    case SPW_LINK_RUN:
        if ((s_ops.status(s_ops.ctx, &running, &s_status_word) != QIRAN_OK) ||
            !running) {
            /*
             * A link that was running and is not is a disconnect, counted
             * separately from a failure to start: the two have different causes
             * and the distinction is worth keeping in telemetry.
             */
            s_disconnects++;
            s_error_word = s_status_word;
            svc_fdir_report(QIRAN_FAULT_SPW_LINK, s_status_word);
            s_state = SPW_LINK_DOWN;
            return;
        }

        deliver_received();
        break;

    case SPW_LINK_FAILED:
    default:
        break;
    }
}

uint32_t comm_spw_link_start_attempts(void)   { return s_attempts; }
uint32_t comm_spw_link_disconnects(void)      { return s_disconnects; }
uint32_t comm_spw_link_errors(void)           { return s_errors; }
uint32_t comm_spw_link_packets_sent(void)     { return s_sent; }
uint32_t comm_spw_link_packets_received(void) { return s_received; }
uint32_t comm_spw_link_status_word(void)      { return s_status_word; }
uint32_t comm_spw_link_last_error_word(void)  { return s_error_word; }
