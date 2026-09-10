#include "qiran/svc/svc_health.h"

#include "qiran/exec/exec_core.h"
#include "qiran/qiran_config.h"
#include "qiran/svc/svc_config.h"
#include "qiran/svc/svc_fdir.h"
#include "qiran/svc/svc_log.h"

typedef struct {
    const char        *name;
    svc_health_check_t check;
    void              *ctx;
    qiran_fault_id_t   fault;
    uint32_t           period;
    uint32_t           tolerance;
    uint32_t           countdown;
    bool               gating;

    uint32_t       runs;
    uint32_t       failures;
    uint32_t       consecutive;
    uint32_t       last_value;
    qiran_status_t last_status;
} monitor_t;

static monitor_t      s_monitor[SVC_HEALTH_MONITOR_MAX];
static uint32_t       s_count;
static qiran_health_t s_state;
static uint32_t       s_transitions;
static uint32_t       s_seen_overruns;

/*
 * Deadline keeping. Already reported by the fault service, so it carries no
 * fault class of its own; it is here because software persistently missing its
 * deadlines is what a processor reset might actually clear.
 */
static qiran_status_t check_exec_timing(void *ctx, uint32_t *value)
{
    exec_stats_t st;

    QIRAN_UNUSED(ctx);
    exec_stats_get(&st);
    *value = st.overrun_events;

    if (st.overrun_events != s_seen_overruns) {
        s_seen_overruns = st.overrun_events;
        return QIRAN_ERR_TIMEOUT;
    }

    return QIRAN_OK;
}

static qiran_status_t check_config_integrity(void *ctx, uint32_t *value)
{
    QIRAN_UNUSED(ctx);
    *value = svc_config_corrections();
    return svc_config_verify();
}

void svc_health_init(void)
{
    s_count = 0U;
    s_state = QIRAN_HEALTH_HEALTHY;
    s_transitions = 0U;
    s_seen_overruns = 0U;

    /*
     * One late cycle is tolerated: the watchdog period is several minor cycles
     * wide for that reason, and withholding a single service is not a reset.
     */
    (void)svc_health_register("exec_timing", check_exec_timing, NULL,
                              SVC_HEALTH_GATING, 1U, 2U, QIRAN_FAULT_NONE);

    (void)svc_health_register("config_integrity", check_config_integrity, NULL,
                              SVC_HEALTH_GATING, QIRAN_MINOR_PER_MAJOR, 1U,
                              QIRAN_FAULT_NONE);
}

qiran_status_t svc_health_register(const char *name,
                                   svc_health_check_t check,
                                   void *ctx,
                                   svc_health_class_t monitor_class,
                                   uint32_t period_cycles,
                                   uint32_t tolerance,
                                   qiran_fault_id_t fault)
{
    monitor_t *m;

    if ((name == NULL) || (check == NULL) || (period_cycles == 0U) ||
        (fault >= QIRAN_FAULT_COUNT)) {
        return QIRAN_ERR_PARAM;
    }
    if (s_count >= SVC_HEALTH_MONITOR_MAX) {
        return QIRAN_ERR_RANGE;
    }

    m = &s_monitor[s_count];

    m->name = name;
    m->check = check;
    m->ctx = ctx;
    m->fault = fault;
    m->period = period_cycles;
    m->tolerance = tolerance;
    m->gating = (monitor_class == SVC_HEALTH_GATING);

    /* Staggered by registration order, so monitors sharing a period spread
       across cycles instead of putting a spike into one. */
    m->countdown = (s_count % period_cycles) + 1U;

    m->runs = 0U;
    m->failures = 0U;
    m->consecutive = 0U;
    m->last_value = 0U;
    m->last_status = QIRAN_OK;

    s_count++;
    return QIRAN_OK;
}

static bool contributing(const monitor_t *m)
{
    return m->gating && (m->consecutive > m->tolerance);
}

void health_check(void)
{
    uint32_t i;

    for (i = 0U; i < s_count; i++) {
        monitor_t *m = &s_monitor[i];
        uint32_t value = 0U;
        qiran_status_t st;

        /* Countdown rather than a modulo: no division in the cyclic path. */
        if (m->countdown > 1U) {
            m->countdown--;
            continue;
        }

        m->countdown = m->period;
        st = m->check(m->ctx, &value);

        m->runs++;
        m->last_value = value;
        m->last_status = st;

        if (st == QIRAN_OK) {
            m->consecutive = 0U;
        } else {
            m->failures++;
            m->consecutive++;
            if (m->fault != QIRAN_FAULT_NONE) {
                svc_fdir_report(m->fault, value);
            }
        }
    }
}

void health_consolidate(void)
{
    qiran_health_t verdict = QIRAN_HEALTH_HEALTHY;
    uint32_t i;

    for (i = 0U; i < s_count; i++) {
        if (contributing(&s_monitor[i])) {
            verdict = QIRAN_HEALTH_FAILED;
        }
    }

    if (verdict != s_state) {
        s_state = verdict;
        s_transitions++;
        svc_log_event((uint16_t)QIRAN_FAULT_NONE, (uint32_t)verdict);
    }
}

qiran_health_t svc_health_state(void)
{
    return s_state;
}

uint32_t svc_health_count(void)
{
    return s_count;
}

uint32_t svc_health_gating_count(void)
{
    uint32_t i;
    uint32_t n = 0U;

    for (i = 0U; i < s_count; i++) {
        if (contributing(&s_monitor[i])) {
            n++;
        }
    }

    return n;
}

uint32_t svc_health_transitions(void)
{
    return s_transitions;
}

bool svc_health_all_passing(void)
{
    uint32_t i;

    for (i = 0U; i < s_count; i++) {
        if (s_monitor[i].last_status != QIRAN_OK) {
            return false;
        }
    }

    return true;
}

bool svc_health_monitor_stat(uint32_t index, svc_health_monitor_stat_t *out)
{
    const monitor_t *m;

    if ((index >= s_count) || (out == NULL)) {
        return false;
    }

    m = &s_monitor[index];

    out->name = m->name;
    out->runs = m->runs;
    out->failures = m->failures;
    out->consecutive = m->consecutive;
    out->last_value = m->last_value;
    out->last_status = m->last_status;
    out->period_cycles = m->period;
    out->tolerance = m->tolerance;
    out->gating = m->gating;
    out->contributing = contributing(m);

    return true;
}
