#include "qiran/exec/exec_core.h"
#include "qiran/mission/mission_seq.h"
#include "qiran/plat/plat_cpu.h"
#include "qiran/plat/plat_irq.h"
#include "qiran/svc/svc_config.h"
#include "qiran/svc/svc_fdir.h"
#include "qiran/svc/svc_health.h"
#include "qiran/svc/svc_log.h"
#include "qiran/svc/svc_time.h"
#include "qiran/svc/svc_watchdog.h"
#include "test_framework.h"

static uint32_t s_flag_calls;
static uint32_t s_reset_calls;

static qiran_status_t fake_report(qiran_fault_id_t id, qiran_severity_t sev,
                                  uint32_t detail)
{
    QIRAN_UNUSED(id);
    QIRAN_UNUSED(sev);
    QIRAN_UNUSED(detail);
    s_flag_calls++;
    return QIRAN_OK;
}

static qiran_status_t fake_reset(qiran_fault_id_t id)
{
    QIRAN_UNUSED(id);
    s_reset_calls++;
    return QIRAN_OK;
}

static const svc_fdir_uplink_t k_uplink = { fake_report, fake_reset };

static void setup(void)
{
    exec_init();
    plat_irq_init();
    svc_log_init();
    svc_fdir_init();
    svc_config_init();
    svc_time_init();
    svc_health_init();
    (void)svc_watchdog_init(NULL);
    mission_seq_init();
    (void)svc_fdir_set_uplink(&k_uplink);
    s_flag_calls = 0U;
    s_reset_calls = 0U;
}

static void tick_ms(uint32_t ms)
{
    uint32_t i;
    for (i = 0U; i < (ms / QIRAN_MINOR_CYCLE_MS); i++) {
        exec_on_minor_tick();
    }
}

/* One pass of the tasks the sequence depends on, in loop order. */
static void cycle(void)
{
    exec_on_minor_tick();
    mission_seq_step();
    error_handling_service();
}

/* --- the sequence --- */

static mission_step_result_t s_result;
static mission_stage_t       s_back_to;
static qiran_fault_id_t      s_fault;
static uint32_t              s_step_calls;

static void scripted(void *ctx, mission_step_t *out)
{
    QIRAN_UNUSED(ctx);
    s_step_calls++;
    out->result = s_result;
    out->back_to = s_back_to;
    out->fault = s_fault;
    out->detail = 0x55U;
}

static void script(mission_step_result_t r)
{
    s_result = r;
    s_back_to = STAGE_BOOT;
    s_fault = QIRAN_FAULT_NONE;
    s_step_calls = 0U;
}

static void register_all(void)
{
    uint32_t i;
    for (i = 0U; i < (uint32_t)STAGE_COUNT; i++) {
        CHECK_TRUE(mission_seq_register((mission_stage_t)i, scripted, NULL)
                   == QIRAN_OK);
    }
}

static void advance_to(mission_stage_t target)
{
    uint32_t guard = 0U;

    script(STEP_DONE);
    while ((mission_stage() != target) && (guard <= (uint32_t)STAGE_COUNT)) {
        mission_seq_step();
        guard++;
    }
    CHECK_EQ_U64(mission_stage(), target);
}

static void test_initial_position(void)
{
    uint32_t i;

    TEST_CASE("the sequence starts at the first stage with nothing behind it");
    setup();
    CHECK_EQ_U64(STAGE_COUNT, 13U);
    CHECK_EQ_U64(mission_stage(), STAGE_BOOT);
    CHECK_EQ_U64(mission_reentries(), 0U);
    CHECK_EQ_U64(mission_stage_attempts(), 0U);
    CHECK_TRUE(!mission_stalled());
    CHECK_TRUE(!mission_aborted());
    CHECK_EQ_U64(mission_payload_status(), QIRAN_PAYLOAD_NON_OPERATIONAL);

    TEST_CASE("every stage is named");
    for (i = 0U; i < (uint32_t)STAGE_COUNT; i++) {
        CHECK_TRUE(mission_stage_name((mission_stage_t)i)[0] != '?');
    }
    CHECK_TRUE(mission_stage_name(STAGE_COUNT)[0] == '?');
    CHECK_EQ_U64(mission_stage_entries(STAGE_COUNT), 0U);

    TEST_CASE("a stage with nothing registered simply does not run");
    cycle();
    CHECK_EQ_U64(mission_stage(), STAGE_BOOT);
}

static void test_registration(void)
{
    TEST_CASE("registration is validated and refuses to replace a stage");
    setup();
    CHECK_TRUE(mission_seq_register(STAGE_COUNT, scripted, NULL)
               == QIRAN_ERR_PARAM);
    CHECK_TRUE(mission_seq_register(STAGE_MRR_TUNE, NULL, NULL)
               == QIRAN_ERR_PARAM);
    CHECK_TRUE(mission_seq_register(STAGE_MRR_TUNE, scripted, NULL)
               == QIRAN_OK);
    CHECK_TRUE(mission_seq_register(STAGE_MRR_TUNE, scripted, NULL)
               == QIRAN_ERR_STATE);
}

static void test_working_and_done(void)
{
    TEST_CASE("a stage still working is called again and does not move on");
    setup();
    register_all();
    script(STEP_WORKING);

    cycle();
    cycle();
    cycle();
    CHECK_EQ_U64(s_step_calls, 3U);
    CHECK_EQ_U64(mission_stage(), STAGE_BOOT);
    CHECK_EQ_U64(mission_stage_entries(STAGE_BOOT), 1U);

    TEST_CASE("finishing moves the sequence to the next stage");
    script(STEP_DONE);
    cycle();
    CHECK_EQ_U64(mission_stage(), STAGE_PRECOND);
    CHECK_EQ_U64(mission_stage_entries(STAGE_PRECOND), 1U);

    TEST_CASE("the sequence walks forward one stage at a time");
    script(STEP_DONE);
    cycle();
    CHECK_EQ_U64(mission_stage(), STAGE_LASER_BRINGUP);

    TEST_CASE("the last stage stays where it is when it finishes");
    advance_to(STAGE_DATA_HANDLING);
    script(STEP_DONE);
    cycle();
    CHECK_EQ_U64(mission_stage(), STAGE_DATA_HANDLING);
}

static void test_retry(void)
{
    TEST_CASE("a failed attempt repeats the stage and counts the attempt");
    setup();
    register_all();
    advance_to(STAGE_MRR_TUNE);
    CHECK_EQ_U64(mission_stage_entries(STAGE_MRR_TUNE), 1U);

    script(STEP_RETRY);
    cycle();
    CHECK_EQ_U64(mission_stage(), STAGE_MRR_TUNE);
    CHECK_EQ_U64(mission_stage_attempts(), 1U);
    CHECK_EQ_U64(mission_stage_entries(STAGE_MRR_TUNE), 2U);
    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_MRR_LOCK), 1U);

    TEST_CASE("a repeat is not counted against the re-entry allowance");
    CHECK_EQ_U64(mission_reentries(), 0U);

    TEST_CASE("spending the retries stalls the stage and reports the flag");
    cycle();
    cycle();
    cycle();
    CHECK_TRUE(mission_stalled());
    CHECK_EQ_U64(mission_stage(), STAGE_MRR_TUNE);
    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_MRR_LOCK),
                 QIRAN_STAGE_RETRY_LIMIT);
    CHECK_EQ_U64(s_flag_calls, 1U);

    TEST_CASE("a stalled stage stops being run at all");
    s_step_calls = 0U;
    cycle();
    cycle();
    CHECK_EQ_U64(s_step_calls, 0U);

    TEST_CASE("finishing clears the count so a later attempt starts fresh");
    setup();
    register_all();
    advance_to(STAGE_MRR_TUNE);
    script(STEP_RETRY);
    cycle();
    CHECK_EQ_U64(mission_stage_attempts(), 1U);
    script(STEP_DONE);
    cycle();
    CHECK_EQ_U64(mission_stage_attempts(), 0U);
    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_MRR_LOCK), 0U);
}

static void test_going_back(void)
{
    TEST_CASE("a stage can send the sequence back to an earlier one");
    setup();
    register_all();
    advance_to(STAGE_SPAD_ENABLE);

    script(STEP_BACK);
    s_back_to = STAGE_CROW_TUNE;
    s_fault = QIRAN_FAULT_CROW_TUNE;
    cycle();

    CHECK_EQ_U64(mission_stage(), STAGE_CROW_TUNE);
    CHECK_EQ_U64(mission_reentries(), 1U);
    CHECK_EQ_U64(svc_fdir_reentry_count(), 1U);
    CHECK_TRUE(!mission_stalled());

    TEST_CASE("going back resets the attempt count for the stage resumed");
    CHECK_EQ_U64(mission_stage_attempts(), 0U);

    TEST_CASE("it cannot be used to skip forward over stages that never ran");
    setup();
    register_all();
    advance_to(STAGE_MRR_TUNE);
    script(STEP_BACK);
    s_back_to = STAGE_EXPERIMENT;
    cycle();
    CHECK_EQ_U64(mission_stage(), STAGE_MRR_TUNE);
    CHECK_TRUE(mission_stalled());

    TEST_CASE("nor to send a stage back to itself");
    setup();
    register_all();
    advance_to(STAGE_MRR_TUNE);
    script(STEP_BACK);
    s_back_to = STAGE_MRR_TUNE;
    cycle();
    CHECK_TRUE(mission_stalled());
    CHECK_EQ_U64(mission_reentries(), 0U);
}

static void test_reentry_allowance(void)
{
    uint32_t i;

    TEST_CASE("the re-entry allowance is global and escalates once spent");
    setup();
    register_all();

    for (i = 0U; i <= QIRAN_REENTRY_LIMIT; i++) {
        advance_to(STAGE_SPAD_ENABLE);
        script(STEP_BACK);
        s_back_to = STAGE_CROW_TUNE;
        s_fault = QIRAN_FAULT_CROW_TUNE;
        cycle();
    }

    CHECK_EQ_U64(mission_reentries(), QIRAN_REENTRY_LIMIT + 1U);
    CHECK_TRUE(svc_fdir_reentry_exceeded());
}

static void test_abort(void)
{
    TEST_CASE("a stage that cannot proceed stalls without spending retries");
    setup();
    register_all();
    advance_to(STAGE_DLI_LOCK);

    script(STEP_ABORT);
    cycle();
    CHECK_TRUE(mission_stalled());
    CHECK_EQ_U64(mission_stage(), STAGE_DLI_LOCK);
    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_DLI_LOCK), 1U);

    TEST_CASE("aborting the run holds it where it stands");
    setup();
    register_all();
    advance_to(STAGE_EXPERIMENT);
    mission_abort(QIRAN_FAULT_OVERCURRENT);

    CHECK_TRUE(mission_aborted());
    CHECK_EQ_U64(mission_abort_cause(), QIRAN_FAULT_OVERCURRENT);
    CHECK_EQ_U64(mission_stage(), STAGE_EXPERIMENT);
    CHECK_EQ_U64(mission_payload_status(), QIRAN_PAYLOAD_NON_OPERATIONAL);

    TEST_CASE("nothing runs afterwards");
    script(STEP_DONE);
    cycle();
    CHECK_EQ_U64(s_step_calls, 0U);
    CHECK_EQ_U64(mission_stage(), STAGE_EXPERIMENT);

    TEST_CASE("aborting twice keeps the first cause");
    mission_abort(QIRAN_FAULT_POST);
    CHECK_EQ_U64(mission_abort_cause(), QIRAN_FAULT_OVERCURRENT);
}

static void test_payload_status(void)
{
    TEST_CASE("the payload is operational once the detectors are enabled");
    setup();
    register_all();
    advance_to(STAGE_SPAD_ENABLE);
    CHECK_EQ_U64(mission_payload_status(), QIRAN_PAYLOAD_NON_OPERATIONAL);

    script(STEP_DONE);
    cycle();
    CHECK_EQ_U64(mission_stage(), STAGE_PVS_T1);
    CHECK_EQ_U64(mission_payload_status(), QIRAN_PAYLOAD_OPERATIONAL);

    TEST_CASE("a later re-entry does not by itself clear it");
    script(STEP_BACK);
    s_back_to = STAGE_CROW_TUNE;
    cycle();
    CHECK_EQ_U64(mission_payload_status(), QIRAN_PAYLOAD_OPERATIONAL);

    TEST_CASE("whoever removes the bias is what clears it");
    mission_payload_status_set(QIRAN_PAYLOAD_NON_OPERATIONAL);
    CHECK_EQ_U64(mission_payload_status(), QIRAN_PAYLOAD_NON_OPERATIONAL);
}

static void test_stage_budget(void)
{
    TEST_CASE("a stage inside its permitted time raises nothing");
    setup();
    register_all();
    advance_to(STAGE_MRR_TUNE);
    script(STEP_WORKING);

    tick_ms(5000U);
    mission_seq_step();
    error_handling_service();
    CHECK_TRUE(!svc_fdir_flag(QIRAN_FAULT_STAGE_TIMEOUT));

    TEST_CASE("exceeding it is reported, once per entry rather than per cycle");
    tick_ms(6000U);
    mission_seq_step();
    mission_seq_step();
    error_handling_service();
    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_STAGE_TIMEOUT), 1U);

    TEST_CASE("re-entering the stage arms the check again");
    script(STEP_RETRY);
    mission_seq_step();
    error_handling_service();
    CHECK_TRUE(mission_stage_elapsed_ms() < 100U);
    script(STEP_WORKING);
    tick_ms(11000U);
    mission_seq_step();
    error_handling_service();
    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_STAGE_TIMEOUT), 2U);

    TEST_CASE("a stage with no specified duration is not timed");
    setup();
    register_all();
    advance_to(STAGE_EXPERIMENT);
    script(STEP_WORKING);
    tick_ms(300000U);
    mission_seq_step();
    error_handling_service();
    CHECK_TRUE(!svc_fdir_flag(QIRAN_FAULT_STAGE_TIMEOUT));
}

/* --- health --- */

static qiran_status_t s_monitor_result;
static uint32_t       s_monitor_calls;

static qiran_status_t monitor(void *ctx, uint32_t *value)
{
    QIRAN_UNUSED(ctx);
    s_monitor_calls++;
    *value = 0x77U;
    return s_monitor_result;
}

static void test_health_registration(void)
{
    TEST_CASE("the built-in monitors are registered at initialisation");
    setup();
    CHECK_EQ_U64(svc_health_count(), 2U);
    CHECK_EQ_U64(svc_health_state(), QIRAN_HEALTH_HEALTHY);

    TEST_CASE("registration is validated");
    CHECK_TRUE(svc_health_register(NULL, monitor, NULL, SVC_HEALTH_ADVISORY,
                                   1U, 0U, QIRAN_FAULT_NONE)
               == QIRAN_ERR_PARAM);
    CHECK_TRUE(svc_health_register("m", NULL, NULL, SVC_HEALTH_ADVISORY,
                                   1U, 0U, QIRAN_FAULT_NONE)
               == QIRAN_ERR_PARAM);
    CHECK_TRUE(svc_health_register("m", monitor, NULL, SVC_HEALTH_ADVISORY,
                                   0U, 0U, QIRAN_FAULT_NONE)
               == QIRAN_ERR_PARAM);
    CHECK_TRUE(svc_health_register("m", monitor, NULL, SVC_HEALTH_ADVISORY,
                                   1U, 0U, QIRAN_FAULT_COUNT)
               == QIRAN_ERR_PARAM);
}

static void test_health_period(void)
{
    uint32_t i;

    TEST_CASE("a monitor runs on its own period, not every cycle");
    setup();
    s_monitor_result = QIRAN_OK;
    s_monitor_calls = 0U;
    CHECK_TRUE(svc_health_register("periodic", monitor, NULL,
                                   SVC_HEALTH_ADVISORY, 5U, 0U,
                                   QIRAN_FAULT_NONE) == QIRAN_OK);

    for (i = 0U; i < 20U; i++) {
        health_check();
    }
    CHECK_EQ_U64(s_monitor_calls, 4U);
}

static void test_health_verdict(void)
{
    uint32_t i;

    TEST_CASE("a gating monitor within its tolerance does not fail the verdict");
    setup();
    s_monitor_result = QIRAN_ERR_HARDWARE;
    s_monitor_calls = 0U;
    CHECK_TRUE(svc_health_register("gate", monitor, NULL, SVC_HEALTH_GATING,
                                   1U, 2U, QIRAN_FAULT_NONE) == QIRAN_OK);

    health_check();
    health_consolidate();
    CHECK_EQ_U64(svc_health_state(), QIRAN_HEALTH_HEALTHY);
    health_check();
    health_consolidate();
    CHECK_EQ_U64(svc_health_state(), QIRAN_HEALTH_HEALTHY);

    TEST_CASE("past its tolerance it does");
    health_check();
    health_consolidate();
    CHECK_EQ_U64(svc_health_state(), QIRAN_HEALTH_FAILED);
    CHECK_EQ_U64(svc_health_gating_count(), 1U);
    CHECK_EQ_U64(svc_health_transitions(), 1U);

    TEST_CASE("one good result restores it");
    s_monitor_result = QIRAN_OK;
    health_check();
    health_consolidate();
    CHECK_EQ_U64(svc_health_state(), QIRAN_HEALTH_HEALTHY);
    CHECK_EQ_U64(svc_health_transitions(), 2U);

    TEST_CASE("an advisory monitor never fails the verdict, however long");
    setup();
    s_monitor_result = QIRAN_ERR_HARDWARE;
    CHECK_TRUE(svc_health_register("advice", monitor, NULL,
                                   SVC_HEALTH_ADVISORY, 1U, 0U,
                                   QIRAN_FAULT_NONE) == QIRAN_OK);
    for (i = 0U; i < 20U; i++) {
        health_check();
        health_consolidate();
    }
    CHECK_EQ_U64(svc_health_state(), QIRAN_HEALTH_HEALTHY);
    CHECK_TRUE(!svc_health_all_passing());

    TEST_CASE("gathering and deciding are separate passes");
    setup();
    s_monitor_result = QIRAN_ERR_HARDWARE;
    CHECK_TRUE(svc_health_register("gate", monitor, NULL, SVC_HEALTH_GATING,
                                   1U, 0U, QIRAN_FAULT_NONE) == QIRAN_OK);
    health_check();
    CHECK_EQ_U64(svc_health_state(), QIRAN_HEALTH_HEALTHY);
    health_consolidate();
    CHECK_EQ_U64(svc_health_state(), QIRAN_HEALTH_FAILED);
}

static void test_health_reporting(void)
{
    svc_health_monitor_stat_t st;

    TEST_CASE("a monitor with a fault class reports, one without does not");
    setup();
    s_monitor_result = QIRAN_ERR_HARDWARE;
    CHECK_TRUE(svc_health_register("reports", monitor, NULL,
                                   SVC_HEALTH_ADVISORY, 1U, 0U,
                                   QIRAN_FAULT_TEMPERATURE) == QIRAN_OK);
    health_check();
    error_handling_service();
    CHECK_TRUE(svc_fdir_flag(QIRAN_FAULT_TEMPERATURE));

    TEST_CASE("its readings are readable");
    CHECK_TRUE(svc_health_monitor_stat(2U, &st));
    CHECK_EQ_U64(st.failures, 1U);
    CHECK_EQ_U64(st.last_value, 0x77U);
    CHECK_TRUE(!st.gating);
    CHECK_TRUE(!svc_health_monitor_stat(99U, &st));
    CHECK_TRUE(!svc_health_monitor_stat(0U, NULL));
}

static void test_watchdog_gate(void)
{
    TEST_CASE("the watchdog is withheld while the verdict is unhealthy");
    setup();
    s_monitor_result = QIRAN_ERR_HARDWARE;
    CHECK_TRUE(svc_health_register("gate", monitor, NULL, SVC_HEALTH_GATING,
                                   1U, 0U, QIRAN_FAULT_NONE) == QIRAN_OK);

    watchdog_tickle_if_healthy();
    CHECK_EQ_U64(svc_watchdog_suppressed_cycles(), 0U);

    health_check();
    health_consolidate();
    watchdog_tickle_if_healthy();
    CHECK_EQ_U64(svc_watchdog_suppressed_cycles(), 1U);

    TEST_CASE("and serviced again once it recovers");
    s_monitor_result = QIRAN_OK;
    health_check();
    health_consolidate();
    watchdog_tickle_if_healthy();
    CHECK_EQ_U64(svc_watchdog_suppressed_cycles(), 1U);
}

int main(void)
{
    plat_cpu_init();

    test_initial_position();
    test_registration();
    test_working_and_done();
    test_retry();
    test_going_back();
    test_reentry_allowance();
    test_abort();
    test_payload_status();
    test_stage_budget();

    test_health_registration();
    test_health_period();
    test_health_verdict();
    test_health_reporting();
    test_watchdog_gate();

    return TEST_REPORT();
}
