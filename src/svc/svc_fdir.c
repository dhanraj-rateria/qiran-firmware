#include "qiran/svc/svc_fdir.h"

#include "qiran/exec/exec_core.h"
#include "qiran/plat/plat_cpu.h"
#include "qiran/plat/plat_irq.h"
#include "qiran/plat/plat_isr_uart.h"
#include "qiran/qiran_config.h"
#include "qiran/svc/svc_log.h"

#define STAGE_RETRY QIRAN_STAGE_RETRY_LIMIT

/*
 * Tier, escalation and limit for every fault class. Over-current, brownout and
 * temperature-out-of-bounds are the cases where the processor keeps running but
 * the payload must be power cycled, so they request a reset directly.
 *
 * The seven lock-acquisition classes carry the stage retry limit as their
 * Critical-tier limit, which is what makes a stage's retry counter and the
 * Critical counter the same counter.
 *
 * OPEN: lock-acquisition classes escalate by reporting a flag, following the
 * capability requirements' wording for retry exhaustion. Read strictly, the
 * error-handling reconciliation would instead make exhaustion request a power
 * reset. The difference matters: a single stubborn stage would then cost a full
 * reset cycle out of the operating window. Confirm with systems engineering.
 */
static const svc_fdir_policy_t k_policy[QIRAN_FAULT_COUNT] = {
    /* NONE                    */ { QIRAN_SEV_MINOR,    QIRAN_ESCALATE_LOG_ONLY,     0U },

    /* BOOT_IMAGE              */ { QIRAN_SEV_SEVERE,   QIRAN_ESCALATE_REPORT,       0U },
    /* POST                    */ { QIRAN_SEV_SEVERE,   QIRAN_ESCALATE_REPORT,       0U },
    /* DEVINIT                 */ { QIRAN_SEV_SEVERE,   QIRAN_ESCALATE_REPORT,       0U },
    /* CYCLE_OVERRUN           */ { QIRAN_SEV_CRITICAL, QIRAN_ESCALATE_REPORT,       5U },
    /* IRQ_OVERFLOW            */ { QIRAN_SEV_MEDIUM,   QIRAN_ESCALATE_LOG_ONLY,     0U },
    /* UART_RX_OVERFLOW        */ { QIRAN_SEV_MINOR,    QIRAN_ESCALATE_LOG_ONLY,     0U },
    /* PL_ERROR                */ { QIRAN_SEV_CRITICAL, QIRAN_ESCALATE_REPORT,       5U },
    /* WATCHDOG_EXPIRY         */ { QIRAN_SEV_SEVERE,   QIRAN_ESCALATE_REPORT,       0U },
    /* MEMORY                  */ { QIRAN_SEV_CRITICAL, QIRAN_ESCALATE_POWER_RESET,  3U },

    /* OVERCURRENT             */ { QIRAN_SEV_SEVERE,   QIRAN_ESCALATE_POWER_RESET,  0U },
    /* BROWNOUT                */ { QIRAN_SEV_SEVERE,   QIRAN_ESCALATE_POWER_RESET,  0U },
    /* TEMPERATURE             */ { QIRAN_SEV_SEVERE,   QIRAN_ESCALATE_POWER_RESET,  0U },

    /* LASER_TEC               */ { QIRAN_SEV_CRITICAL, QIRAN_ESCALATE_REPORT, STAGE_RETRY },
    /* LASER_POWER             */ { QIRAN_SEV_CRITICAL, QIRAN_ESCALATE_REPORT, STAGE_RETRY },
    /* MRR_LOCK                */ { QIRAN_SEV_CRITICAL, QIRAN_ESCALATE_REPORT, STAGE_RETRY },
    /* CROW_TUNE               */ { QIRAN_SEV_CRITICAL, QIRAN_ESCALATE_REPORT, STAGE_RETRY },
    /* UMZI_TUNE               */ { QIRAN_SEV_CRITICAL, QIRAN_ESCALATE_REPORT, STAGE_RETRY },
    /* DLI_LOCK                */ { QIRAN_SEV_CRITICAL, QIRAN_ESCALATE_REPORT, STAGE_RETRY },
    /* MRR_RELOCK              */ { QIRAN_SEV_CRITICAL, QIRAN_ESCALATE_REPORT, STAGE_RETRY },

    /* SPAD_OPTICAL_POWER      */ { QIRAN_SEV_SEVERE,   QIRAN_ESCALATE_REPORT,       0U },
    /* SPAD_DARK_COUNT         */ { QIRAN_SEV_MEDIUM,   QIRAN_ESCALATE_LOG_ONLY,     0U },

    /* SPW_LINK                */ { QIRAN_SEV_CRITICAL, QIRAN_ESCALATE_REPORT,       3U },
    /* PACKET_INTEGRITY        */ { QIRAN_SEV_MEDIUM,   QIRAN_ESCALATE_LOG_ONLY,     0U },

    /* DDR_OVERRUN             */ { QIRAN_SEV_CRITICAL, QIRAN_ESCALATE_REPORT,       3U },
    /* NAND_WRITE              */ { QIRAN_SEV_CRITICAL, QIRAN_ESCALATE_REPORT,       3U }
};

QIRAN_STATIC_ASSERT(QIRAN_ARRAY_LEN(k_policy) == (size_t)QIRAN_FAULT_COUNT,
                    every_fault_class_has_a_policy);

/* Reported by any context, serviced only by the loop; the difference is new. */
static volatile uint32_t s_reported[QIRAN_FAULT_COUNT];
static uint32_t          s_serviced[QIRAN_FAULT_COUNT];
static volatile uint32_t s_detail[QIRAN_FAULT_COUNT];
static volatile uint32_t s_tick[QIRAN_FAULT_COUNT];

static svc_fdir_record_t s_record[QIRAN_FAULT_COUNT];
static uint32_t          s_by_severity[QIRAN_SEV_COUNT];

static svc_fdir_uplink_t s_uplink;
static bool              s_uplink_set;
static uint32_t          s_undelivered;

static uint32_t s_reentries;

/* Platform counters this service collects rather than being pushed. */
static uint32_t s_seen_overruns;
static uint32_t s_seen_uart_overflow;
static uint32_t s_seen_irq_dropped[PLAT_IRQ_COUNT];

void svc_fdir_init(void)
{
    uint32_t i;

    for (i = 0U; i < (uint32_t)QIRAN_FAULT_COUNT; i++) {
        s_reported[i] = 0U;
        s_serviced[i] = 0U;
        s_detail[i] = 0U;
        s_tick[i] = 0U;
        s_record[i].occurrences = 0U;
        s_record[i].escalations = 0U;
        s_record[i].last_tick = 0U;
        s_record[i].last_detail = 0U;
        s_record[i].flag = false;
        s_record[i].elevated = false;
    }

    for (i = 0U; i < (uint32_t)QIRAN_SEV_COUNT; i++) {
        s_by_severity[i] = 0U;
    }

    for (i = 0U; i < (uint32_t)PLAT_IRQ_COUNT; i++) {
        s_seen_irq_dropped[i] = 0U;
    }

    s_uplink.report_flag = NULL;
    s_uplink.request_power_reset = NULL;
    s_uplink_set = false;
    s_undelivered = 0U;
    s_reentries = 0U;
    s_seen_overruns = 0U;
    s_seen_uart_overflow = 0U;
}

qiran_status_t svc_fdir_set_uplink(const svc_fdir_uplink_t *uplink)
{
    if (uplink == NULL) {
        s_uplink.report_flag = NULL;
        s_uplink.request_power_reset = NULL;
        s_uplink_set = false;
        return QIRAN_OK;
    }

    if ((uplink->report_flag == NULL) || (uplink->request_power_reset == NULL)) {
        return QIRAN_ERR_PARAM;
    }

    s_uplink = *uplink;
    s_uplink_set = true;
    return QIRAN_OK;
}

void svc_fdir_report(qiran_fault_id_t id, uint32_t detail)
{
    uint32_t prior;

    if (id >= QIRAN_FAULT_COUNT) {
        return;
    }

    /*
     * Several interrupts as well as the loop can report the same class, which
     * is more than one producer for this counter, so the increment is guarded.
     * The guarded region is three stores.
     */
    prior = plat_cpu_irq_disable();
    s_reported[id]++;
    s_detail[id] = detail;
    s_tick[id] = exec_raw_tick_count();
    plat_cpu_irq_restore(prior);
}

static void escalate(qiran_fault_id_t id, uint32_t detail)
{
    const svc_fdir_policy_t *p = &k_policy[id];
    qiran_status_t st = QIRAN_ERR_UNSUPPORTED;

    s_record[id].escalations++;

    switch (p->escalation) {
    case QIRAN_ESCALATE_REPORT:
        if (s_uplink_set) {
            st = s_uplink.report_flag(id, p->severity, detail);
        }
        break;

    case QIRAN_ESCALATE_POWER_RESET:
        /*
         * The request is raised and execution continues: a processor still able
         * to run keeps running, and the decision to cycle power is the
         * observer's. Nothing here suspends operation.
         */
        if (s_uplink_set) {
            st = s_uplink.request_power_reset(id);
        }
        break;

    case QIRAN_ESCALATE_LOG_ONLY:
    default:
        st = QIRAN_OK;
        break;
    }

    if (st != QIRAN_OK) {
        s_undelivered++;
    }
}

/*
 * One log entry per class per pass, carrying the number of occurrences seen in
 * that pass, so a fault repeating every cycle cannot flood the log or make this
 * service's execution time depend on fault rate. Every tier logs.
 */
static void classify(qiran_fault_id_t id, uint32_t occurrences)
{
    const svc_fdir_policy_t *p = &k_policy[id];
    svc_fdir_record_t *r = &s_record[id];
    uint32_t detail = s_detail[id];

    r->occurrences += occurrences;
    r->last_detail = detail;
    r->last_tick = s_tick[id];
    r->flag = true;

    s_by_severity[p->severity] += occurrences;

    svc_log_fault(id, p->severity, detail);

    switch (p->severity) {
    case QIRAN_SEV_SEVERE:
        escalate(id, detail);
        break;

    case QIRAN_SEV_CRITICAL:
        /*
         * Tolerated below the limit, then elevated and escalated once. Further
         * occurrences do not re-escalate until the class is cleared.
         */
        if ((p->limit != 0U) && (r->occurrences >= (uint32_t)p->limit) &&
            !r->elevated) {
            r->elevated = true;
            escalate(id, detail);
        }
        break;

    case QIRAN_SEV_MEDIUM:
    case QIRAN_SEV_MINOR:
    default:
        /*
         * Flag and log only. Correction of a degraded-performance fault is
         * decided on the ground and applied by command; nothing is corrected
         * autonomously here.
         */
        break;
    }
}

static void collect_platform_counters(void)
{
    exec_stats_t ex;
    uint32_t now;
    uint32_t i;

    exec_stats_get(&ex);
    if (ex.overrun_events != s_seen_overruns) {
        uint32_t delta = ex.overrun_events - s_seen_overruns;

        s_seen_overruns = ex.overrun_events;
        for (i = 0U; i < delta; i++) {
            svc_fdir_report(QIRAN_FAULT_CYCLE_OVERRUN, ex.overrun_cycles_lost);
        }
    }

    now = plat_isr_uart_overflow();
    if (now != s_seen_uart_overflow) {
        s_seen_uart_overflow = now;
        svc_fdir_report(QIRAN_FAULT_UART_RX_OVERFLOW, now);
    }

    for (i = 0U; i < (uint32_t)PLAT_IRQ_COUNT; i++) {
        plat_irq_stats_t st;

        plat_irq_stats_get((plat_irq_id_t)i, &st);
        if (st.dropped != s_seen_irq_dropped[i]) {
            s_seen_irq_dropped[i] = st.dropped;
            svc_fdir_report(QIRAN_FAULT_IRQ_OVERFLOW, i);
        }
    }
}

static void drain_interrupt_faults(void)
{
    plat_irq_event_t ev;

    while (plat_irq_take(PLAT_IRQ_PL_ERROR, &ev)) {
        svc_fdir_report(QIRAN_FAULT_PL_ERROR, ev.datum);
    }

    while (plat_irq_take(PLAT_IRQ_WATCHDOG, &ev)) {
        svc_fdir_report(QIRAN_FAULT_WATCHDOG_EXPIRY, ev.datum);
    }
}

void error_handling_service(void)
{
    uint32_t i;

    drain_interrupt_faults();
    collect_platform_counters();

    for (i = 1U; i < (uint32_t)QIRAN_FAULT_COUNT; i++) {
        uint32_t reported = s_reported[i];
        uint32_t nnew = reported - s_serviced[i];

        if (nnew == 0U) {
            continue;
        }

        s_serviced[i] = reported;
        classify((qiran_fault_id_t)i, nnew);
    }
}

uint32_t svc_fdir_occurrences(qiran_fault_id_t id)
{
    return (id < QIRAN_FAULT_COUNT) ? s_record[id].occurrences : 0U;
}

bool svc_fdir_retries_exhausted(qiran_fault_id_t id)
{
    if (id >= QIRAN_FAULT_COUNT) {
        return false;
    }
    if (k_policy[id].limit == 0U) {
        return false;
    }
    return s_record[id].occurrences >= (uint32_t)k_policy[id].limit;
}

void svc_fdir_clear(qiran_fault_id_t id)
{
    uint32_t prior;

    if (id >= QIRAN_FAULT_COUNT) {
        return;
    }

    prior = plat_cpu_irq_disable();
    s_serviced[id] = s_reported[id];
    plat_cpu_irq_restore(prior);

    s_record[id].occurrences = 0U;
    s_record[id].flag = false;
    s_record[id].elevated = false;
}

bool svc_fdir_flag(qiran_fault_id_t id)
{
    return (id < QIRAN_FAULT_COUNT) && s_record[id].flag;
}

void svc_fdir_record_get(qiran_fault_id_t id, svc_fdir_record_t *out)
{
    if ((id < QIRAN_FAULT_COUNT) && (out != NULL)) {
        *out = s_record[id];
    }
}

const svc_fdir_policy_t *svc_fdir_policy(qiran_fault_id_t id)
{
    return (id < QIRAN_FAULT_COUNT) ? &k_policy[id] : NULL;
}

uint32_t svc_fdir_severity_count(qiran_severity_t severity)
{
    return (severity < QIRAN_SEV_COUNT) ? s_by_severity[severity] : 0U;
}

uint32_t svc_fdir_undelivered(void)
{
    return s_undelivered;
}

void svc_fdir_reentry_note(qiran_fault_id_t cause)
{
    s_reentries++;
    svc_log_event((uint16_t)cause, s_reentries);

    if (s_reentries > QIRAN_REENTRY_LIMIT) {
        escalate(cause, s_reentries);
    }
}

uint32_t svc_fdir_reentry_count(void)
{
    return s_reentries;
}

bool svc_fdir_reentry_exceeded(void)
{
    return s_reentries > QIRAN_REENTRY_LIMIT;
}
