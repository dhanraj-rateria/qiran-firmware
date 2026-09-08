#include "../../src/mission/mission_state.c"

#include "qiran/plat/plat_cpu.h"
#include "qiran/plat/plat_irq.h"
#include "test_framework.h"

static void setup(void)
{
    exec_init();
    plat_irq_init();
    svc_log_init();
    svc_fdir_init();
    svc_config_init();
    svc_time_init();
    mission_state_init();
}

static void tick_ms(uint32_t ms)
{
    uint32_t i;
    for (i = 0U; i < (ms / QIRAN_MINOR_CYCLE_MS); i++) {
        exec_on_minor_tick();
    }
}

/* Walks the nominal path so later tests can start from a chosen state. */
static void advance_to(mission_state_id_t target)
{
    static const mission_state_id_t k_path[] = {
        SPR_PRECOND, SPR_LASER_BRINGUP, SPR_MRR_TUNE, SPR_CROW_TUNE,
        SPR_UMZI_TUNE, SPR_DLI_LOCK, SPR_SPAD_ENABLE, SPR_PVS_T1,
        SPR_EXPERIMENT, SPR_PROCESS, SPR_DATA_HANDLING
    };
    uint32_t i;

    for (i = 0U; i < QIRAN_ARRAY_LEN(k_path); i++) {
        if (mission_state_current() == target) {
            return;
        }
        if (mission_state_request(k_path[i]) != QIRAN_OK) {
            return;
        }
    }
}

static void test_model_shape(void)
{
    uint32_t i;
    uint32_t reentries = 0U;

    TEST_CASE("the model holds fifteen named states and starts in boot");
    setup();
    CHECK_EQ_U64(SPR_COUNT, 15U);
    CHECK_EQ_U64(mission_state_current(), SPR_BOOT);
    CHECK_EQ_U64(mission_payload_status(), QIRAN_PAYLOAD_NON_OPERATIONAL);
    CHECK_TRUE(!mission_state_terminated());

    for (i = 0U; i < (uint32_t)SPR_COUNT; i++) {
        CHECK_TRUE(mission_state_name((mission_state_id_t)i) != NULL);
        CHECK_TRUE(mission_state_name((mission_state_id_t)i)[0] != '?');
    }
    CHECK_TRUE(mission_state_name(SPR_COUNT)[0] == '?');

    TEST_CASE("every transition names real states and has a reason");
    for (i = 0U; i < QIRAN_ARRAY_LEN(k_transition); i++) {
        CHECK_TRUE(k_transition[i].from < (uint8_t)SPR_COUNT);
        CHECK_TRUE(k_transition[i].to < (uint8_t)SPR_COUNT);
        CHECK_TRUE(k_transition[i].reason != NULL);
        if (k_transition[i].reentry) {
            reentries++;
        }
    }

    TEST_CASE("no transition is listed twice");
    {
        uint32_t j;
        uint32_t duplicates = 0U;

        for (i = 0U; i < QIRAN_ARRAY_LEN(k_transition); i++) {
            for (j = i + 1U; j < QIRAN_ARRAY_LEN(k_transition); j++) {
                if ((k_transition[i].from == k_transition[j].from) &&
                    (k_transition[i].to == k_transition[j].to)) {
                    duplicates++;
                }
            }
        }
        CHECK_EQ_U64(duplicates, 0U);
    }

    TEST_CASE("a retry of the same stage is not counted as a re-entry");
    for (i = 0U; i < QIRAN_ARRAY_LEN(k_transition); i++) {
        if (k_transition[i].from == k_transition[i].to) {
            CHECK_TRUE(!k_transition[i].reentry);
        }
    }

    TEST_CASE("every re-entry goes to a stage earlier than the one it leaves");
    for (i = 0U; i < QIRAN_ARRAY_LEN(k_transition); i++) {
        if (k_transition[i].reentry &&
            (k_transition[i].from != (uint8_t)SPR_CAL)) {
            CHECK_TRUE(k_transition[i].to < k_transition[i].from);
        }
    }
    CHECK_EQ_U64(reentries, 12U);
}

static void test_no_safe_or_fault_state_exists(void)
{
    uint32_t i;

    TEST_CASE("no state is a safe or fault state, by name or by having no exit");
    setup();

    /* Every state except the two the model deliberately leaves open must have
       at least one transition out of it. */
    for (i = 0U; i < (uint32_t)SPR_COUNT; i++) {
        bool has_exit = false;
        uint32_t j;

        for (j = 0U; j < QIRAN_ARRAY_LEN(k_transition); j++) {
            if (k_transition[j].from == (uint8_t)i) {
                has_exit = true;
            }
        }

        if ((i != (uint32_t)SPR_PVS_T3) && (i != (uint32_t)SPR_DATA_HANDLING)) {
            CHECK_TRUE(has_exit);
        }
    }

    TEST_CASE("the extended characterisation state is unreachable as specified");
    for (i = 0U; i < QIRAN_ARRAY_LEN(k_transition); i++) {
        CHECK_TRUE(k_transition[i].to != (uint8_t)SPR_PVS_T3);
    }
}

static void test_legality(void)
{
    TEST_CASE("transitions the model contains are legal");
    setup();
    CHECK_TRUE(mission_state_transition_legal(SPR_BOOT, SPR_PRECOND));
    CHECK_TRUE(mission_state_transition_legal(SPR_BOOT, SPR_BOOT));
    CHECK_TRUE(mission_state_transition_legal(SPR_SPAD_ENABLE, SPR_PVS_T1));
    CHECK_TRUE(mission_state_transition_legal(SPR_EXPERIMENT, SPR_DLI_LOCK));

    TEST_CASE("transitions it does not contain are not");
    CHECK_TRUE(!mission_state_transition_legal(SPR_BOOT, SPR_EXPERIMENT));
    CHECK_TRUE(!mission_state_transition_legal(SPR_PRECOND, SPR_PRECOND));
    CHECK_TRUE(!mission_state_transition_legal(SPR_PVS_T1, SPR_PVS_T3));
    CHECK_TRUE(!mission_state_transition_legal(SPR_DATA_HANDLING, SPR_BOOT));
    CHECK_TRUE(!mission_state_transition_legal(SPR_COUNT, SPR_BOOT));
    CHECK_TRUE(!mission_state_transition_legal(SPR_BOOT, SPR_COUNT));
}

static void test_request_accepts_and_refuses(void)
{
    TEST_CASE("a legal request moves the state and counts the entry");
    setup();
    CHECK_TRUE(mission_state_request(SPR_PRECOND) == QIRAN_OK);
    CHECK_EQ_U64(mission_state_current(), SPR_PRECOND);
    CHECK_EQ_U64(mission_state_entries(SPR_PRECOND), 1U);
    CHECK_EQ_U64(mission_state_rejections(), 0U);

    TEST_CASE("an illegal request is refused, reported and leaves the state alone");
    CHECK_TRUE(mission_state_request(SPR_EXPERIMENT) == QIRAN_ERR_STATE);
    CHECK_EQ_U64(mission_state_current(), SPR_PRECOND);
    CHECK_EQ_U64(mission_state_rejections(), 1U);
    error_handling_service();
    CHECK_TRUE(svc_fdir_flag(QIRAN_FAULT_STATE_TRANSITION));

    TEST_CASE("a request naming no real state is refused");
    CHECK_TRUE(mission_state_request(SPR_COUNT) == QIRAN_ERR_PARAM);
    CHECK_EQ_U64(mission_state_rejections(), 2U);

    TEST_CASE("a retry of the same stage is accepted and counted");
    setup();
    advance_to(SPR_MRR_TUNE);
    CHECK_EQ_U64(mission_state_entries(SPR_MRR_TUNE), 1U);
    CHECK_TRUE(mission_state_request(SPR_MRR_TUNE) == QIRAN_OK);
    CHECK_TRUE(mission_state_request(SPR_MRR_TUNE) == QIRAN_OK);
    CHECK_EQ_U64(mission_state_entries(SPR_MRR_TUNE), 3U);
    CHECK_EQ_U64(mission_state_reentries(), 0U);
}

static void test_payload_status(void)
{
    TEST_CASE("payload is non-operational until the interlocks have passed");
    setup();
    advance_to(SPR_SPAD_ENABLE);
    CHECK_EQ_U64(mission_payload_status(), QIRAN_PAYLOAD_NON_OPERATIONAL);

    TEST_CASE("it becomes operational on leaving detector enable for tier one");
    CHECK_TRUE(mission_state_request(SPR_PVS_T1) == QIRAN_OK);
    CHECK_EQ_U64(mission_payload_status(), QIRAN_PAYLOAD_OPERATIONAL);

    TEST_CASE("a drift re-entry does not by itself clear operational status");
    CHECK_TRUE(mission_state_request(SPR_EXPERIMENT) == QIRAN_OK);
    CHECK_TRUE(mission_state_request(SPR_CROW_TUNE) == QIRAN_OK);
    CHECK_EQ_U64(mission_payload_status(), QIRAN_PAYLOAD_OPERATIONAL);

    TEST_CASE("whoever removes the bias is what clears it");
    mission_payload_status_set(QIRAN_PAYLOAD_NON_OPERATIONAL);
    CHECK_EQ_U64(mission_payload_status(), QIRAN_PAYLOAD_NON_OPERATIONAL);
}

static void test_reentry_counting(void)
{
    TEST_CASE("a return to an earlier stage counts once, globally");
    setup();
    advance_to(SPR_CROW_TUNE);
    CHECK_EQ_U64(mission_state_reentries(), 0U);

    CHECK_TRUE(mission_state_request(SPR_MRR_TUNE) == QIRAN_OK);
    CHECK_EQ_U64(mission_state_reentries(), 1U);
    CHECK_EQ_U64(svc_fdir_reentry_count(), 1U);

    TEST_CASE("the allowance is shared across stages and escalates when spent");
    CHECK_TRUE(mission_state_request(SPR_CROW_TUNE) == QIRAN_OK);
    CHECK_TRUE(mission_state_request(SPR_MRR_TUNE) == QIRAN_OK);
    CHECK_TRUE(mission_state_request(SPR_CROW_TUNE) == QIRAN_OK);
    CHECK_TRUE(mission_state_request(SPR_UMZI_TUNE) == QIRAN_OK);
    CHECK_TRUE(mission_state_request(SPR_DLI_LOCK) == QIRAN_OK);
    CHECK_TRUE(mission_state_request(SPR_CROW_TUNE) == QIRAN_OK);
    CHECK_TRUE(mission_state_request(SPR_MRR_TUNE) == QIRAN_OK);

    CHECK_EQ_U64(mission_state_reentries(), 4U);
    CHECK_TRUE(!svc_fdir_reentry_exceeded());

    CHECK_TRUE(mission_state_request(SPR_CROW_TUNE) == QIRAN_OK);
    CHECK_TRUE(mission_state_request(SPR_MRR_TUNE) == QIRAN_OK);
    CHECK_EQ_U64(mission_state_reentries(), 5U);
    CHECK_TRUE(!svc_fdir_reentry_exceeded());

    CHECK_TRUE(mission_state_request(SPR_CROW_TUNE) == QIRAN_OK);
    CHECK_TRUE(mission_state_request(SPR_MRR_TUNE) == QIRAN_OK);
    CHECK_EQ_U64(mission_state_reentries(), 6U);
    CHECK_TRUE(svc_fdir_reentry_exceeded());
}

static void test_state_budget_enforcement(void)
{
    TEST_CASE("a state within its permitted time raises nothing");
    setup();
    advance_to(SPR_MRR_TUNE);
    tick_ms(5000U);
    state_machine_update();
    CHECK_TRUE(!svc_fdir_flag(QIRAN_FAULT_STATE_TIMEOUT));
    CHECK_TRUE(mission_state_elapsed_ms() >= 5000U);

    TEST_CASE("exceeding it is reported");
    tick_ms(6000U);
    state_machine_update();
    error_handling_service();
    CHECK_TRUE(svc_fdir_flag(QIRAN_FAULT_STATE_TIMEOUT));
    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_STATE_TIMEOUT), 1U);

    TEST_CASE("it is reported once per entry, not once per cycle");
    tick_ms(2000U);
    state_machine_update();
    state_machine_update();
    error_handling_service();
    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_STATE_TIMEOUT), 1U);

    TEST_CASE("re-entering the state arms the check again");
    CHECK_TRUE(mission_state_request(SPR_MRR_TUNE) == QIRAN_OK);
    CHECK_TRUE(mission_state_elapsed_ms() < 100U);
    tick_ms(11000U);
    state_machine_update();
    error_handling_service();
    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_STATE_TIMEOUT), 2U);

    TEST_CASE("a state with no specified duration is not policed");
    setup();
    CHECK_TRUE(mission_state_request(SPR_PRECOND) == QIRAN_OK);
    tick_ms(300000U);
    state_machine_update();
    CHECK_TRUE(!svc_fdir_flag(QIRAN_FAULT_STATE_TIMEOUT));
}

static void test_termination(void)
{
    TEST_CASE("terminating holds the current state and does not enter another");
    setup();
    advance_to(SPR_PVS_T1);
    CHECK_TRUE(mission_state_request(SPR_PVS_T2) == QIRAN_OK);

    mission_state_terminate(QIRAN_FAULT_MRR_RELOCK);
    CHECK_TRUE(mission_state_terminated());
    CHECK_EQ_U64(mission_state_current(), SPR_PVS_T2);
    CHECK_EQ_U64(mission_state_terminal_cause(), QIRAN_FAULT_MRR_RELOCK);
    CHECK_EQ_U64(mission_payload_status(), QIRAN_PAYLOAD_NON_OPERATIONAL);

    TEST_CASE("no further transition is accepted, legal or not");
    CHECK_TRUE(mission_state_request(SPR_EXPERIMENT) == QIRAN_ERR_STATE);
    CHECK_TRUE(mission_state_request(SPR_CAL) == QIRAN_ERR_STATE);
    CHECK_EQ_U64(mission_state_current(), SPR_PVS_T2);

    TEST_CASE("the periodic update does nothing once terminated");
    tick_ms(300000U);
    state_machine_update();
    CHECK_TRUE(!svc_fdir_flag(QIRAN_FAULT_STATE_TIMEOUT));

    TEST_CASE("terminating twice keeps the first cause");
    mission_state_terminate(QIRAN_FAULT_POST);
    CHECK_EQ_U64(mission_state_terminal_cause(), QIRAN_FAULT_MRR_RELOCK);
}

static void test_history(void)
{
    mission_history_t h[MISSION_HISTORY_DEPTH + 4U];
    uint32_t n;

    TEST_CASE("history is empty before anything has moved");
    setup();
    CHECK_EQ_U64(mission_state_history(h, QIRAN_ARRAY_LEN(h)), 0U);
    CHECK_EQ_U64(mission_state_history(NULL, 4U), 0U);

    TEST_CASE("history reads back most recent first");
    CHECK_TRUE(mission_state_request(SPR_PRECOND) == QIRAN_OK);
    CHECK_TRUE(mission_state_request(SPR_LASER_BRINGUP) == QIRAN_OK);
    CHECK_TRUE(mission_state_request(SPR_MRR_TUNE) == QIRAN_OK);

    n = mission_state_history(h, QIRAN_ARRAY_LEN(h));
    CHECK_EQ_U64(n, 3U);
    CHECK_EQ_U64(h[0].from, SPR_LASER_BRINGUP);
    CHECK_EQ_U64(h[0].to, SPR_MRR_TUNE);
    CHECK_EQ_U64(h[1].from, SPR_PRECOND);
    CHECK_EQ_U64(h[2].from, SPR_BOOT);

    TEST_CASE("a re-entry is marked in the record");
    CHECK_TRUE(mission_state_request(SPR_CROW_TUNE) == QIRAN_OK);
    CHECK_TRUE(mission_state_request(SPR_MRR_TUNE) == QIRAN_OK);
    n = mission_state_history(h, QIRAN_ARRAY_LEN(h));
    CHECK_EQ_U64(h[0].reentry, 1U);
    CHECK_EQ_U64(h[1].reentry, 0U);

    TEST_CASE("history keeps the most recent entries once full");
    setup();
    {
        uint32_t i;
        for (i = 0U; i < 40U; i++) {
            CHECK_TRUE(mission_state_request(SPR_BOOT) == QIRAN_OK);
        }
    }
    n = mission_state_history(h, QIRAN_ARRAY_LEN(h));
    CHECK_EQ_U64(n, MISSION_HISTORY_DEPTH);
    CHECK_EQ_U64(h[0].from, SPR_BOOT);
    CHECK_EQ_U64(h[0].to, SPR_BOOT);

    TEST_CASE("a smaller buffer is filled without overrunning");
    n = mission_state_history(h, 3U);
    CHECK_EQ_U64(n, 3U);
}

int main(void)
{
    plat_cpu_init();

    test_model_shape();
    test_no_safe_or_fault_state_exists();
    test_legality();
    test_request_accepts_and_refuses();
    test_payload_status();
    test_reentry_counting();
    test_state_budget_enforcement();
    test_termination();
    test_history();

    return TEST_REPORT();
}
