/*
 * Both the fault service and the interrupt framework are compiled in: the
 * former so the policy table can be asserted against directly, the latter so an
 * interrupt-published fault can be injected without a controller.
 */
#include "../../src/plat/plat_irq.c"
#include "../../src/svc/svc_fdir.c"

#include "qiran/svc/svc_log.h"
#include "test_framework.h"

static uint32_t         s_flag_calls;
static uint32_t         s_reset_calls;
static qiran_fault_id_t s_last_flag_id;
static qiran_severity_t s_last_flag_sev;
static qiran_fault_id_t s_last_reset_id;

static qiran_status_t fake_report(qiran_fault_id_t id, qiran_severity_t sev,
                                  uint32_t detail)
{
    QIRAN_UNUSED(detail);
    s_flag_calls++;
    s_last_flag_id = id;
    s_last_flag_sev = sev;
    return QIRAN_OK;
}

static qiran_status_t fake_reset(qiran_fault_id_t id)
{
    s_reset_calls++;
    s_last_reset_id = id;
    return QIRAN_OK;
}

static const svc_fdir_uplink_t k_uplink = { fake_report, fake_reset };

static uint32_t s_ack_datum;

static uint32_t test_ack(void *ctx, uint16_t *kind)
{
    QIRAN_UNUSED(ctx);
    *kind = 0U;
    return s_ack_datum;
}

static void setup(bool with_uplink)
{
    exec_init();
    plat_irq_init();
    svc_log_init();
    svc_fdir_init();

    s_flag_calls = 0U;
    s_reset_calls = 0U;

    if (with_uplink) {
        (void)svc_fdir_set_uplink(&k_uplink);
    }
}

static void tick(uint32_t n)
{
    uint32_t i;
    for (i = 0U; i < n; i++) {
        exec_on_minor_tick();
    }
}

/* --- policy table --- */

static void test_stage_retry_counters_are_the_critical_counters(void)
{
    static const qiran_fault_id_t k_stage[] = {
        QIRAN_FAULT_LASER_TEC, QIRAN_FAULT_LASER_POWER, QIRAN_FAULT_MRR_LOCK,
        QIRAN_FAULT_CROW_TUNE, QIRAN_FAULT_UMZI_TUNE, QIRAN_FAULT_DLI_LOCK,
        QIRAN_FAULT_MRR_RELOCK
    };
    uint32_t i;

    TEST_CASE("every lock-acquisition class is Critical with the stage retry limit");

    CHECK_EQ_U64(QIRAN_ARRAY_LEN(k_stage), 7U);

    for (i = 0U; i < QIRAN_ARRAY_LEN(k_stage); i++) {
        const svc_fdir_policy_t *p = svc_fdir_policy(k_stage[i]);

        CHECK_TRUE(p != NULL);
        CHECK_EQ_U64(p->severity, QIRAN_SEV_CRITICAL);
        CHECK_EQ_U64(p->limit, QIRAN_STAGE_RETRY_LIMIT);
    }

    TEST_CASE("faults where the processor keeps running request a power cycle");
    CHECK_EQ_U64(svc_fdir_policy(QIRAN_FAULT_OVERCURRENT)->escalation,
                 QIRAN_ESCALATE_POWER_RESET);
    CHECK_EQ_U64(svc_fdir_policy(QIRAN_FAULT_TEMPERATURE)->escalation,
                 QIRAN_ESCALATE_POWER_RESET);
    CHECK_EQ_U64(svc_fdir_policy(QIRAN_FAULT_BROWNOUT)->escalation,
                 QIRAN_ESCALATE_POWER_RESET);
    CHECK_EQ_U64(svc_fdir_policy(QIRAN_FAULT_OVERCURRENT)->severity,
                 QIRAN_SEV_SEVERE);
}

/* --- the four tiers --- */

static void test_minor_flags_and_logs_only(void)
{
    TEST_CASE("a warning sets a flag and logs, and changes nothing else");
    setup(true);

    svc_fdir_report(QIRAN_FAULT_UART_RX_OVERFLOW, 0x11U);
    error_handling_service();

    CHECK_TRUE(svc_fdir_flag(QIRAN_FAULT_UART_RX_OVERFLOW));
    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_UART_RX_OVERFLOW), 1U);
    CHECK_EQ_U64(svc_log_fault_total(), 1U);
    CHECK_EQ_U64(s_flag_calls, 0U);
    CHECK_EQ_U64(s_reset_calls, 0U);
}

static void test_medium_does_not_self_correct(void)
{
    uint32_t i;

    TEST_CASE("a degraded-performance fault never escalates, however often it recurs");
    setup(true);

    for (i = 0U; i < 20U; i++) {
        svc_fdir_report(QIRAN_FAULT_SPAD_DARK_COUNT, i);
        error_handling_service();
    }

    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_SPAD_DARK_COUNT), 20U);
    CHECK_EQ_U64(svc_fdir_severity_count(QIRAN_SEV_MEDIUM), 20U);
    CHECK_EQ_U64(s_flag_calls, 0U);
    CHECK_EQ_U64(s_reset_calls, 0U);
    CHECK_EQ_U64(svc_log_fault_total(), 20U);
}

static void test_critical_tolerates_then_elevates_once(void)
{
    svc_fdir_record_t r;
    uint32_t i;

    TEST_CASE("a Critical fault is tolerated below its limit");
    setup(true);

    for (i = 0U; i < (QIRAN_STAGE_RETRY_LIMIT - 1U); i++) {
        svc_fdir_report(QIRAN_FAULT_LASER_TEC, 0x22U);
        error_handling_service();
        CHECK_TRUE(!svc_fdir_retries_exhausted(QIRAN_FAULT_LASER_TEC));
        CHECK_EQ_U64(s_flag_calls, 0U);
    }
    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_LASER_TEC),
                 QIRAN_STAGE_RETRY_LIMIT - 1U);

    TEST_CASE("reaching the limit elevates it and escalates exactly once");
    svc_fdir_report(QIRAN_FAULT_LASER_TEC, 0x33U);
    error_handling_service();

    CHECK_TRUE(svc_fdir_retries_exhausted(QIRAN_FAULT_LASER_TEC));
    CHECK_EQ_U64(s_flag_calls, 1U);
    CHECK_EQ_U64(s_last_flag_id, QIRAN_FAULT_LASER_TEC);
    CHECK_EQ_U64(s_last_flag_sev, QIRAN_SEV_CRITICAL);
    CHECK_EQ_U64(s_reset_calls, 0U);

    svc_fdir_record_get(QIRAN_FAULT_LASER_TEC, &r);
    CHECK_TRUE(r.elevated);
    CHECK_EQ_U64(r.escalations, 1U);
    CHECK_EQ_U64(r.last_detail, 0x33U);

    TEST_CASE("further occurrences do not escalate again until cleared");
    for (i = 0U; i < 5U; i++) {
        svc_fdir_report(QIRAN_FAULT_LASER_TEC, 0x44U);
        error_handling_service();
    }
    CHECK_EQ_U64(s_flag_calls, 1U);

    TEST_CASE("clearing on success lets the retry count start again from zero");
    svc_fdir_clear(QIRAN_FAULT_LASER_TEC);
    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_LASER_TEC), 0U);
    CHECK_TRUE(!svc_fdir_flag(QIRAN_FAULT_LASER_TEC));
    CHECK_TRUE(!svc_fdir_retries_exhausted(QIRAN_FAULT_LASER_TEC));

    svc_fdir_report(QIRAN_FAULT_LASER_TEC, 0x55U);
    error_handling_service();
    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_LASER_TEC), 1U);
    CHECK_EQ_U64(s_flag_calls, 1U);
}

static void test_critical_reported_in_a_burst(void)
{
    TEST_CASE("a burst reaching the limit within one pass still escalates once");
    setup(true);

    svc_fdir_report(QIRAN_FAULT_MRR_LOCK, 1U);
    svc_fdir_report(QIRAN_FAULT_MRR_LOCK, 2U);
    svc_fdir_report(QIRAN_FAULT_MRR_LOCK, 3U);
    svc_fdir_report(QIRAN_FAULT_MRR_LOCK, 4U);
    error_handling_service();

    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_MRR_LOCK), 4U);
    CHECK_EQ_U64(s_flag_calls, 1U);
    /* Coalesced into one entry so log volume cannot track fault rate. */
    CHECK_EQ_U64(svc_log_fault_total(), 1U);
}

static void test_severe_escalates_immediately(void)
{
    TEST_CASE("a severe fault escalates in the same service pass");
    setup(true);

    svc_fdir_report(QIRAN_FAULT_OVERCURRENT, 0x99U);
    error_handling_service();

    CHECK_EQ_U64(s_reset_calls, 1U);
    CHECK_EQ_U64(s_last_reset_id, QIRAN_FAULT_OVERCURRENT);
    CHECK_EQ_U64(svc_fdir_severity_count(QIRAN_SEV_SEVERE), 1U);
    CHECK_EQ_U64(svc_log_fault_total(), 1U);

    TEST_CASE("execution continues: nothing is suspended by the escalation");
    svc_fdir_report(QIRAN_FAULT_OVERCURRENT, 0xAAU);
    error_handling_service();
    CHECK_EQ_U64(s_reset_calls, 2U);
}

static void test_every_tier_logs(void)
{
    TEST_CASE("all four tiers produce a log entry, not only the lower two");
    setup(true);

    svc_fdir_report(QIRAN_FAULT_UART_RX_OVERFLOW, 0U);
    svc_fdir_report(QIRAN_FAULT_SPAD_DARK_COUNT, 0U);
    svc_fdir_report(QIRAN_FAULT_LASER_TEC, 0U);
    svc_fdir_report(QIRAN_FAULT_OVERCURRENT, 0U);
    error_handling_service();

    CHECK_EQ_U64(svc_log_fault_total(), 4U);
    CHECK_EQ_U64(svc_fdir_severity_count(QIRAN_SEV_MINOR), 1U);
    CHECK_EQ_U64(svc_fdir_severity_count(QIRAN_SEV_MEDIUM), 1U);
    CHECK_EQ_U64(svc_fdir_severity_count(QIRAN_SEV_CRITICAL), 1U);
    CHECK_EQ_U64(svc_fdir_severity_count(QIRAN_SEV_SEVERE), 1U);
}

static void test_undelivered_escalation_is_visible(void)
{
    TEST_CASE("escalation with no uplink registered is counted, not silent");
    setup(false);

    svc_fdir_report(QIRAN_FAULT_OVERCURRENT, 0U);
    error_handling_service();

    CHECK_EQ_U64(svc_fdir_undelivered(), 1U);
    CHECK_EQ_U64(svc_log_fault_total(), 1U);
}

/* --- faults the service collects rather than being told about --- */

static void test_executive_overrun_becomes_a_fault(void)
{
    TEST_CASE("a missed deadline is picked up from the executive and classified");
    setup(true);

    tick(1U);
    exec_wait_for_minor_tick();
    error_handling_service();
    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_CYCLE_OVERRUN), 0U);

    /* Loop body outran its slot by two cycles. */
    tick(3U);
    exec_wait_for_minor_tick();
    error_handling_service();

    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_CYCLE_OVERRUN), 1U);
    CHECK_TRUE(svc_fdir_flag(QIRAN_FAULT_CYCLE_OVERRUN));
}

static void test_interrupt_published_fault_is_drained(void)
{
    plat_irq_source_t src;
    svc_fdir_record_t rec;
    uint32_t i;

    TEST_CASE("a fabric error raised in interrupt context reaches the fault path");
    setup(true);

    src.irq_id = 63U;
    src.trigger = PLAT_GIC_TRIGGER_LEVEL_HIGH;
    src.ack = NULL;
    CHECK_TRUE(plat_irq_register(PLAT_IRQ_PL_ERROR, &src) == QIRAN_ERR_PARAM);

    s_ack_datum = 0x5A5AU;
    src.ack = test_ack;
    CHECK_TRUE(plat_irq_register(PLAT_IRQ_PL_ERROR, &src) == QIRAN_OK);

    irq_handler(&s_slot[PLAT_IRQ_PL_ERROR]);
    irq_handler(&s_slot[PLAT_IRQ_PL_ERROR]);

    error_handling_service();

    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_PL_ERROR), 2U);
    svc_fdir_record_get(QIRAN_FAULT_PL_ERROR, &rec);
    CHECK_EQ_U64(rec.last_detail, 0x5A5AU);

    TEST_CASE("interrupt queue overflow is itself reported as a fault");
    for (i = 0U; i < (QIRAN_IRQ_QUEUE_DEPTH + 2U); i++) {
        irq_handler(&s_slot[PLAT_IRQ_PL_ERROR]);
    }
    error_handling_service();
    CHECK_TRUE(svc_fdir_flag(QIRAN_FAULT_IRQ_OVERFLOW));
}

/* --- re-entry accounting --- */

static void test_reentry_limit(void)
{
    uint32_t i;

    TEST_CASE("re-entries are counted globally and escalate past the limit");
    setup(true);

    for (i = 0U; i < QIRAN_REENTRY_LIMIT; i++) {
        svc_fdir_reentry_note(QIRAN_FAULT_MRR_LOCK);
        CHECK_TRUE(!svc_fdir_reentry_exceeded());
        CHECK_EQ_U64(s_flag_calls, 0U);
    }
    CHECK_EQ_U64(svc_fdir_reentry_count(), QIRAN_REENTRY_LIMIT);

    svc_fdir_reentry_note(QIRAN_FAULT_DLI_LOCK);
    CHECK_TRUE(svc_fdir_reentry_exceeded());
    CHECK_EQ_U64(s_flag_calls, 1U);
    CHECK_EQ_U64(s_last_flag_id, QIRAN_FAULT_DLI_LOCK);
}

/* --- log --- */

static void test_log_order_and_overflow(void)
{
    svc_log_record_t rec[QIRAN_LOG_DEPTH + 8U];
    uint32_t i;
    uint32_t n;

    TEST_CASE("log preserves order and reports totals");
    svc_log_init();
    CHECK_EQ_U64(svc_log_count(), 0U);

    for (i = 0U; i < 5U; i++) {
        svc_log_event((uint16_t)(0x100U + i), i);
    }
    CHECK_EQ_U64(svc_log_count(), 5U);
    CHECK_EQ_U64(svc_log_total(), 5U);
    CHECK_EQ_U64(svc_log_fault_total(), 0U);

    n = svc_log_read(rec, QIRAN_ARRAY_LEN(rec));
    CHECK_EQ_U64(n, 5U);
    for (i = 0U; i < 5U; i++) {
        CHECK_EQ_U64(rec[i].code, 0x100U + i);
        CHECK_EQ_U64(rec[i].detail, i);
        CHECK_EQ_U64(rec[i].category, SVC_LOG_EVENT);
    }
    CHECK_EQ_U64(svc_log_count(), 0U);

    TEST_CASE("when full the oldest records are kept and drops are counted");
    svc_log_init();
    for (i = 0U; i < (QIRAN_LOG_DEPTH + 6U); i++) {
        svc_log_event((uint16_t)i, i);
    }
    CHECK_EQ_U64(svc_log_count(), QIRAN_LOG_DEPTH);
    CHECK_EQ_U64(svc_log_dropped(), 6U);
    CHECK_EQ_U64(svc_log_total(), QIRAN_LOG_DEPTH + 6U);

    n = svc_log_read(rec, QIRAN_ARRAY_LEN(rec));
    CHECK_EQ_U64(n, QIRAN_LOG_DEPTH);
    CHECK_EQ_U64(rec[0].code, 0U);
    CHECK_EQ_U64(rec[QIRAN_LOG_DEPTH - 1U].code, QIRAN_LOG_DEPTH - 1U);

    TEST_CASE("fault entries carry their tier and are counted separately");
    svc_log_init();
    svc_log_fault(QIRAN_FAULT_OVERCURRENT, QIRAN_SEV_SEVERE, 0x77U);
    svc_log_trace(9U, 0U);
    CHECK_EQ_U64(svc_log_fault_total(), 1U);
    CHECK_EQ_U64(svc_log_total(), 2U);

    n = svc_log_read(rec, QIRAN_ARRAY_LEN(rec));
    CHECK_EQ_U64(n, 2U);
    CHECK_EQ_U64(rec[0].category, SVC_LOG_FAULT);
    CHECK_EQ_U64(rec[0].code, QIRAN_FAULT_OVERCURRENT);
    CHECK_EQ_U64(rec[0].severity, QIRAN_SEV_SEVERE);
    CHECK_EQ_U64(rec[1].category, SVC_LOG_TRACE);
}

int main(void)
{
    plat_cpu_init();

    test_stage_retry_counters_are_the_critical_counters();

    test_minor_flags_and_logs_only();
    test_medium_does_not_self_correct();
    test_critical_tolerates_then_elevates_once();
    test_critical_reported_in_a_burst();
    test_severe_escalates_immediately();
    test_every_tier_logs();
    test_undelivered_escalation_is_visible();

    test_executive_overrun_becomes_a_fault();
    test_interrupt_published_fault_is_drained();
    test_reentry_limit();
    test_log_order_and_overflow();

    return TEST_REPORT();
}
