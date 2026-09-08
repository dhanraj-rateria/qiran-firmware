#include "../../src/mission/mission_ops_seq.c"

#include "qiran/exec/exec_core.h"
#include "qiran/photonic/photonic_sched.h"
#include "qiran/plat/plat_cpu.h"
#include "qiran/plat/plat_irq.h"
#include "qiran/svc/svc_config.h"
#include "qiran/svc/svc_time.h"
#include "test_framework.h"

static uint32_t         s_flag_calls;
static uint32_t         s_reset_calls;
static qiran_fault_id_t s_last_flag;

static qiran_status_t fake_report(qiran_fault_id_t id, qiran_severity_t sev,
                                  uint32_t detail)
{
    QIRAN_UNUSED(sev);
    QIRAN_UNUSED(detail);
    s_flag_calls++;
    s_last_flag = id;
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
    mission_state_init();
    mission_ops_init();
    photonic_sched_init();
    (void)svc_fdir_set_uplink(&k_uplink);
    s_flag_calls = 0U;
    s_reset_calls = 0U;
}

/* One full loop pass: the state slot then the fault slot, in loop order. */
static void cycle(void)
{
    exec_on_minor_tick();
    state_machine_update();
    error_handling_service();
}

/* Steps the nominal path with a passing stage-zero check. */
static void advance_to(mission_state_id_t target);

/* --- stage 0 --- */

static qiran_status_t s_health_result;
static qiran_status_t s_comms_result;
static uint32_t       s_health_calls;
static uint32_t       s_comms_calls;

static qiran_status_t health_check(uint32_t *detail)
{
    s_health_calls++;
    *detail = 0xA1U;
    return s_health_result;
}

static qiran_status_t comms_check(uint32_t *detail)
{
    s_comms_calls++;
    *detail = 0xB2U;
    return s_comms_result;
}

static const mission_precond_checks_t k_checks = { health_check, comms_check };

static void enter_precond(void)
{
    CHECK_TRUE(mission_state_request(SPR_PRECOND) == QIRAN_OK);
}

/* Resumes from wherever the state already sits on the nominal path. */
static void advance_to(mission_state_id_t target)
{
    static const mission_state_id_t k_path[] = {
        SPR_PRECOND, SPR_LASER_BRINGUP, SPR_MRR_TUNE, SPR_CROW_TUNE,
        SPR_UMZI_TUNE, SPR_DLI_LOCK, SPR_SPAD_ENABLE
    };
    uint32_t start = 0U;
    uint32_t i;

    for (i = 0U; i < QIRAN_ARRAY_LEN(k_path); i++) {
        if (k_path[i] == mission_state_current()) {
            start = i + 1U;
            break;
        }
    }

    for (i = start; i < QIRAN_ARRAY_LEN(k_path); i++) {
        if (mission_state_current() == target) {
            return;
        }
        if (mission_state_request(k_path[i]) != QIRAN_OK) {
            return;
        }
    }
}

static void advance_to_mrr(void)
{
    advance_to(SPR_MRR_TUNE);
    CHECK_EQ_U64(mission_state_current(), SPR_MRR_TUNE);
}

static void advance_to_spad(void)
{
    advance_to(SPR_SPAD_ENABLE);
    CHECK_EQ_U64(mission_state_current(), SPR_SPAD_ENABLE);
}

static void test_precond_is_driven_and_passes(void)
{
    TEST_CASE("stage zero is driven, and boot is not");
    setup();
    CHECK_TRUE(mission_ops_driven(SPR_PRECOND));
    CHECK_TRUE(!mission_ops_driven(SPR_BOOT));
    CHECK_TRUE(!mission_ops_driven(SPR_MRR_TUNE));

    TEST_CASE("nothing is driven while the state is boot");
    cycle();
    CHECK_EQ_U64(mission_ops_steps(), 0U);
    CHECK_EQ_U64(mission_state_current(), SPR_BOOT);

    TEST_CASE("both checks run and passing them advances to laser bring-up");
    enter_precond();
    (void)mission_precond_set_checks(&k_checks);
    s_health_result = QIRAN_OK;
    s_comms_result = QIRAN_OK;
    s_health_calls = 0U;
    s_comms_calls = 0U;

    cycle();
    CHECK_EQ_U64(s_health_calls, 1U);
    CHECK_EQ_U64(s_comms_calls, 1U);
    CHECK_EQ_U64(mission_state_current(), SPR_LASER_BRINGUP);
    CHECK_EQ_U64(mission_ops_steps(), 1U);
    CHECK_EQ_U64(s_flag_calls, 0U);
    CHECK_TRUE(!mission_ops_stalled());
}

static void test_precond_check_order(void)
{
    TEST_CASE("a failed health check stops before the communication check");
    setup();
    enter_precond();
    (void)mission_precond_set_checks(&k_checks);
    s_health_result = QIRAN_ERR_HARDWARE;
    s_comms_result = QIRAN_OK;
    s_health_calls = 0U;
    s_comms_calls = 0U;

    cycle();
    CHECK_EQ_U64(s_health_calls, 1U);
    CHECK_EQ_U64(s_comms_calls, 0U);
    CHECK_EQ_U64(mission_state_current(), SPR_PRECOND);

    TEST_CASE("a failed communication check is reported in its own right");
    setup();
    enter_precond();
    (void)mission_precond_set_checks(&k_checks);
    s_health_result = QIRAN_OK;
    s_comms_result = QIRAN_ERR_TIMEOUT;
    s_comms_calls = 0U;

    cycle();
    CHECK_EQ_U64(s_comms_calls, 1U);
    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_PRECOND), 1U);
    CHECK_TRUE(svc_fdir_flag(QIRAN_FAULT_PRECOND));
}

static void test_absent_checks_are_not_a_pass(void)
{
    TEST_CASE("with no checks supplied the precondition is not declared met");
    setup();
    enter_precond();
    (void)mission_precond_set_checks(NULL);

    cycle();
    CHECK_EQ_U64(mission_state_current(), SPR_PRECOND);
    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_PRECOND), 1U);

    TEST_CASE("half-supplied checks are refused at registration");
    {
        mission_precond_checks_t half = { health_check, NULL };
        CHECK_TRUE(mission_precond_set_checks(&half) == QIRAN_ERR_PARAM);
    }
}

static void test_retries_are_bounded_then_the_stage_stalls(void)
{
    uint32_t i;

    TEST_CASE("the stage retries exactly its limit, no more");
    setup();
    enter_precond();
    (void)mission_precond_set_checks(&k_checks);
    s_health_result = QIRAN_ERR_HARDWARE;
    s_health_calls = 0U;

    for (i = 0U; i < 10U; i++) {
        cycle();
    }

    CHECK_EQ_U64(s_health_calls, QIRAN_STAGE_RETRY_LIMIT);
    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_PRECOND),
                 QIRAN_STAGE_RETRY_LIMIT);

    TEST_CASE("spending them reports the flag to the observer exactly once");
    CHECK_EQ_U64(s_flag_calls, 1U);
    CHECK_EQ_U64(s_last_flag, QIRAN_FAULT_PRECOND);
    CHECK_EQ_U64(s_reset_calls, 0U);

    TEST_CASE("the state is held rather than changed, and driving stops");
    CHECK_TRUE(mission_ops_stalled());
    CHECK_EQ_U64(mission_ops_stalled_state(), SPR_PRECOND);
    CHECK_EQ_U64(mission_state_current(), SPR_PRECOND);
    CHECK_TRUE(!mission_state_terminated());

    TEST_CASE("no self transition was invented for a stage that has none");
    CHECK_TRUE(!mission_state_transition_legal(SPR_PRECOND, SPR_PRECOND));
    CHECK_EQ_U64(mission_state_entries(SPR_PRECOND), 1U);
    CHECK_EQ_U64(mission_state_rejections(), 0U);
}

/* --- the sequencer, against stages that do not exist yet --- */

static stage_result_t     s_scripted;
static mission_state_id_t s_scripted_target;
static qiran_fault_id_t   s_scripted_fault;
static uint32_t           s_scripted_calls;

static void scripted_step(void *ctx, stage_outcome_t *out)
{
    QIRAN_UNUSED(ctx);
    s_scripted_calls++;
    out->result = s_scripted;
    out->target = s_scripted_target;
    out->fault = s_scripted_fault;
    out->detail = 0x77U;
}

static void test_registration_rules(void)
{
    TEST_CASE("registration is validated and refuses to replace a stage");
    setup();
    CHECK_TRUE(mission_ops_register(SPR_COUNT, scripted_step, NULL)
               == QIRAN_ERR_PARAM);
    CHECK_TRUE(mission_ops_register(SPR_MRR_TUNE, NULL, NULL)
               == QIRAN_ERR_PARAM);
    CHECK_TRUE(mission_ops_register(SPR_MRR_TUNE, scripted_step, NULL)
               == QIRAN_OK);
    CHECK_TRUE(mission_ops_register(SPR_MRR_TUNE, scripted_step, NULL)
               == QIRAN_ERR_STATE);
    CHECK_TRUE(mission_ops_register(SPR_PRECOND, scripted_step, NULL)
               == QIRAN_ERR_STATE);
}

static void test_busy_keeps_the_stage(void)
{
    TEST_CASE("a stage still working is called again and does not advance");
    setup();
    CHECK_TRUE(mission_ops_register(SPR_MRR_TUNE, scripted_step, NULL)
               == QIRAN_OK);
    advance_to_mrr();

    s_scripted = STAGE_BUSY;
    s_scripted_fault = QIRAN_FAULT_NONE;
    s_scripted_calls = 0U;

    cycle();
    cycle();
    cycle();
    CHECK_EQ_U64(s_scripted_calls, 3U);
    CHECK_EQ_U64(mission_state_current(), SPR_MRR_TUNE);
    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_MRR_LOCK), 0U);
}

static void test_retry_uses_the_self_transition_where_one_exists(void)
{
    TEST_CASE("a stage with a retry transition re-enters itself");
    setup();
    CHECK_TRUE(mission_ops_register(SPR_MRR_TUNE, scripted_step, NULL)
               == QIRAN_OK);
    advance_to_mrr();
    CHECK_EQ_U64(mission_state_entries(SPR_MRR_TUNE), 1U);

    s_scripted = STAGE_RETRY;
    s_scripted_fault = QIRAN_FAULT_NONE;
    cycle();
    CHECK_EQ_U64(mission_state_entries(SPR_MRR_TUNE), 2U);
    CHECK_EQ_U64(mission_state_current(), SPR_MRR_TUNE);
    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_MRR_LOCK), 1U);

    TEST_CASE("re-entering a stage is not counted against the re-entry limit");
    CHECK_EQ_U64(mission_state_reentries(), 0U);

    TEST_CASE("passing later clears the retry count for a future attempt");
    s_scripted = STAGE_PASS;
    cycle();
    CHECK_EQ_U64(mission_state_current(), SPR_CROW_TUNE);
    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_MRR_LOCK), 0U);
}

static void test_redirect_is_checked_against_the_model(void)
{
    TEST_CASE("a redirect the model allows is taken and counted as a re-entry");
    setup();
    CHECK_TRUE(mission_ops_register(SPR_SPAD_ENABLE, scripted_step, NULL)
               == QIRAN_OK);
    advance_to_spad();

    s_scripted = STAGE_REDIRECT;
    s_scripted_target = SPR_CROW_TUNE;
    s_scripted_fault = QIRAN_FAULT_CROW_TUNE;
    cycle();

    CHECK_EQ_U64(mission_state_current(), SPR_CROW_TUNE);
    CHECK_EQ_U64(mission_state_reentries(), 1U);
    CHECK_TRUE(!mission_ops_stalled());

    TEST_CASE("a redirect the model does not allow is refused and stalls");
    setup();
    CHECK_TRUE(mission_ops_register(SPR_MRR_TUNE, scripted_step, NULL)
               == QIRAN_OK);
    advance_to_mrr();

    s_scripted = STAGE_REDIRECT;
    s_scripted_target = SPR_EXPERIMENT;
    s_scripted_fault = QIRAN_FAULT_NONE;
    cycle();

    CHECK_EQ_U64(mission_state_current(), SPR_MRR_TUNE);
    CHECK_TRUE(mission_ops_stalled());
    CHECK_EQ_U64(mission_state_rejections(), 1U);
    CHECK_TRUE(svc_fdir_flag(QIRAN_FAULT_STATE_TRANSITION));
}

static void test_fail_stalls_immediately(void)
{
    TEST_CASE("a stage that cannot proceed stalls without using its retries");
    setup();
    CHECK_TRUE(mission_ops_register(SPR_MRR_TUNE, scripted_step, NULL)
               == QIRAN_OK);
    advance_to_mrr();

    s_scripted = STAGE_FAIL;
    s_scripted_fault = QIRAN_FAULT_NONE;
    s_scripted_calls = 0U;
    cycle();
    cycle();

    CHECK_EQ_U64(s_scripted_calls, 1U);
    CHECK_TRUE(mission_ops_stalled());
    CHECK_EQ_U64(mission_ops_stalled_state(), SPR_MRR_TUNE);
    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_MRR_LOCK), 1U);
}

static void test_terminated_run_is_not_driven(void)
{
    TEST_CASE("no stage is driven once the run has ended");
    setup();
    CHECK_TRUE(mission_ops_register(SPR_MRR_TUNE, scripted_step, NULL)
               == QIRAN_OK);
    advance_to_mrr();
    mission_state_terminate(QIRAN_FAULT_PRECOND);

    s_scripted = STAGE_PASS;
    s_scripted_calls = 0U;
    cycle();
    CHECK_EQ_U64(s_scripted_calls, 0U);
    CHECK_EQ_U64(mission_state_current(), SPR_MRR_TUNE);
}

/* --- control loop framework --- */

static uint32_t       s_loop_calls[PHOTONIC_LOOP_COUNT];
static qiran_status_t s_loop_result[PHOTONIC_LOOP_COUNT];

static qiran_status_t loop_fn(void *ctx)
{
    photonic_loop_id_t id = (photonic_loop_id_t)(uintptr_t)ctx;

    s_loop_calls[id]++;
    return s_loop_result[id];
}

static void register_loop(photonic_loop_id_t id, qiran_fault_id_t fault)
{
    CHECK_TRUE(photonic_sched_register(id, NULL, loop_fn,
                                       (void *)(uintptr_t)id, fault) == QIRAN_OK);
    s_loop_calls[id] = 0U;
    s_loop_result[id] = QIRAN_OK;
}

static void test_loop_registration(void)
{
    TEST_CASE("loop registration is validated and refuses to replace a loop");
    setup();
    CHECK_TRUE(photonic_sched_register(PHOTONIC_LOOP_COUNT, NULL, loop_fn, NULL,
                                       QIRAN_FAULT_CONTROL_LOOP)
               == QIRAN_ERR_PARAM);
    CHECK_TRUE(photonic_sched_register(PHOTONIC_LOOP_MRR, NULL, NULL, NULL,
                                       QIRAN_FAULT_CONTROL_LOOP)
               == QIRAN_ERR_PARAM);
    CHECK_TRUE(photonic_sched_register(PHOTONIC_LOOP_MRR, NULL, loop_fn, NULL,
                                       QIRAN_FAULT_COUNT)
               == QIRAN_ERR_PARAM);

    register_loop(PHOTONIC_LOOP_MRR, QIRAN_FAULT_MRR_LOCK);
    CHECK_TRUE(photonic_sched_register(PHOTONIC_LOOP_MRR, NULL, loop_fn, NULL,
                                       QIRAN_FAULT_MRR_LOCK)
               == QIRAN_ERR_STATE);

    TEST_CASE("a loop that does not exist cannot be enabled");
    CHECK_TRUE(photonic_sched_enable(PHOTONIC_LOOP_DLI1) == QIRAN_ERR_UNSUPPORTED);
    CHECK_TRUE(!photonic_sched_enabled(PHOTONIC_LOOP_DLI1));
    CHECK_TRUE(photonic_sched_enable(PHOTONIC_LOOP_COUNT) == QIRAN_ERR_PARAM);
}

static void test_only_enabled_loops_run(void)
{
    photonic_loop_stat_t st;

    TEST_CASE("a registered loop does not run until it is enabled");
    setup();
    register_loop(PHOTONIC_LOOP_LASER_TEC, QIRAN_FAULT_LASER_TEC);
    control_loop_service_calls();
    CHECK_EQ_U64(s_loop_calls[PHOTONIC_LOOP_LASER_TEC], 0U);
    CHECK_EQ_U64(photonic_sched_enabled_count(), 0U);

    TEST_CASE("enabling it runs it every pass");
    CHECK_TRUE(photonic_sched_enable(PHOTONIC_LOOP_LASER_TEC) == QIRAN_OK);
    control_loop_service_calls();
    control_loop_service_calls();
    CHECK_EQ_U64(s_loop_calls[PHOTONIC_LOOP_LASER_TEC], 2U);
    CHECK_EQ_U64(photonic_sched_enabled_count(), 1U);

    TEST_CASE("several loops run together");
    register_loop(PHOTONIC_LOOP_DLI1, QIRAN_FAULT_DLI_LOCK);
    register_loop(PHOTONIC_LOOP_DLI2, QIRAN_FAULT_DLI_LOCK);
    CHECK_TRUE(photonic_sched_enable(PHOTONIC_LOOP_DLI1) == QIRAN_OK);
    CHECK_TRUE(photonic_sched_enable(PHOTONIC_LOOP_DLI2) == QIRAN_OK);
    control_loop_service_calls();
    CHECK_EQ_U64(s_loop_calls[PHOTONIC_LOOP_LASER_TEC], 3U);
    CHECK_EQ_U64(s_loop_calls[PHOTONIC_LOOP_DLI1], 1U);
    CHECK_EQ_U64(s_loop_calls[PHOTONIC_LOOP_DLI2], 1U);
    CHECK_EQ_U64(photonic_sched_enabled_count(), 3U);

    TEST_CASE("disabling one leaves the others running");
    photonic_sched_disable(PHOTONIC_LOOP_DLI1);
    control_loop_service_calls();
    CHECK_EQ_U64(s_loop_calls[PHOTONIC_LOOP_DLI1], 1U);
    CHECK_EQ_U64(s_loop_calls[PHOTONIC_LOOP_DLI2], 2U);

    TEST_CASE("its execution time is measured");
    photonic_loop_stat_get(PHOTONIC_LOOP_DLI2, &st);
    CHECK_EQ_U64(st.runs, 2U);
    CHECK_TRUE(st.registered);
    CHECK_TRUE(st.enabled);
    CHECK_TRUE(st.cycles_worst >= st.cycles_last);

    TEST_CASE("disabling all of them stops every loop");
    photonic_sched_disable_all();
    CHECK_EQ_U64(photonic_sched_enabled_count(), 0U);
    control_loop_service_calls();
    CHECK_EQ_U64(s_loop_calls[PHOTONIC_LOOP_DLI2], 2U);
}

static void test_loops_outlast_the_stage_that_started_them(void)
{
    TEST_CASE("a loop keeps running as the state moves on past its stage");
    setup();
    register_loop(PHOTONIC_LOOP_LASER_TEC, QIRAN_FAULT_LASER_TEC);
    advance_to(SPR_LASER_BRINGUP);
    CHECK_TRUE(photonic_sched_enable(PHOTONIC_LOOP_LASER_TEC) == QIRAN_OK);

    control_loop_service_calls();
    advance_to(SPR_SPAD_ENABLE);
    control_loop_service_calls();
    control_loop_service_calls();

    CHECK_EQ_U64(mission_state_current(), SPR_SPAD_ENABLE);
    CHECK_EQ_U64(s_loop_calls[PHOTONIC_LOOP_LASER_TEC], 3U);
    CHECK_TRUE(photonic_sched_enabled(PHOTONIC_LOOP_LASER_TEC));
}

static void test_loop_failures_are_reported(void)
{
    photonic_loop_stat_t st;
    uint32_t i;

    TEST_CASE("a failing iteration is counted and reported");
    setup();
    register_loop(PHOTONIC_LOOP_MRR, QIRAN_FAULT_MRR_LOCK);
    CHECK_TRUE(photonic_sched_enable(PHOTONIC_LOOP_MRR) == QIRAN_OK);

    s_loop_result[PHOTONIC_LOOP_MRR] = QIRAN_ERR_HARDWARE;
    control_loop_service_calls();
    error_handling_service();

    photonic_loop_stat_get(PHOTONIC_LOOP_MRR, &st);
    CHECK_EQ_U64(st.failures, 1U);
    CHECK_EQ_U64(st.consecutive_failures, 1U);
    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_MRR_LOCK), 1U);

    TEST_CASE("one good iteration clears the consecutive count");
    s_loop_result[PHOTONIC_LOOP_MRR] = QIRAN_OK;
    control_loop_service_calls();
    photonic_loop_stat_get(PHOTONIC_LOOP_MRR, &st);
    CHECK_EQ_U64(st.consecutive_failures, 0U);
    CHECK_EQ_U64(st.failures, 1U);

    TEST_CASE("persistent failure escalates through the fault class, once");
    s_loop_result[PHOTONIC_LOOP_MRR] = QIRAN_ERR_HARDWARE;
    for (i = 0U; i < 10U; i++) {
        control_loop_service_calls();
        error_handling_service();
    }
    photonic_loop_stat_get(PHOTONIC_LOOP_MRR, &st);
    CHECK_EQ_U64(st.consecutive_failures, 10U);
    CHECK_EQ_U64(s_flag_calls, 1U);
    CHECK_EQ_U64(s_last_flag, QIRAN_FAULT_MRR_LOCK);

    TEST_CASE("re-enabling a loop restarts its consecutive count");
    photonic_sched_disable(PHOTONIC_LOOP_MRR);
    CHECK_TRUE(photonic_sched_enable(PHOTONIC_LOOP_MRR) == QIRAN_OK);
    photonic_loop_stat_get(PHOTONIC_LOOP_MRR, &st);
    CHECK_EQ_U64(st.consecutive_failures, 0U);

    TEST_CASE("every loop has a name");
    for (i = 0U; i < (uint32_t)PHOTONIC_LOOP_COUNT; i++) {
        CHECK_TRUE(photonic_loop_name((photonic_loop_id_t)i)[0] != '?');
    }
    CHECK_TRUE(photonic_loop_name(PHOTONIC_LOOP_COUNT)[0] == '?');
}

int main(void)
{
    plat_cpu_init();

    test_precond_is_driven_and_passes();
    test_precond_check_order();
    test_absent_checks_are_not_a_pass();
    test_retries_are_bounded_then_the_stage_stalls();
    test_registration_rules();
    test_busy_keeps_the_stage();
    test_retry_uses_the_self_transition_where_one_exists();
    test_redirect_is_checked_against_the_model();
    test_fail_stalls_immediately();
    test_terminated_run_is_not_driven();

    test_loop_registration();
    test_only_enabled_loops_run();
    test_loops_outlast_the_stage_that_started_them();
    test_loop_failures_are_reported();

    return TEST_REPORT();
}
