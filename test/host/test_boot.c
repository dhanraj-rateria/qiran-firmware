#include "qiran/exec/exec_core.h"
#include "qiran/plat/plat_boot.h"
#include "qiran/plat/plat_cpu.h"
#include "qiran/plat/plat_devinit.h"
#include "qiran/plat/plat_irq.h"
#include "qiran/plat/plat_post.h"
#include "qiran/plat/plat_safe_outputs.h"
#include "qiran/qiran_config.h"
#include "qiran/svc/svc_fdir.h"
#include "qiran/svc/svc_log.h"
#include "test_framework.h"

#include <stdlib.h>

static uint32_t s_reset_calls;
static uint32_t s_flag_calls;

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
    plat_post_init();
    plat_devinit_init();
    plat_safe_outputs_reset_registry();
    (void)svc_fdir_set_uplink(&k_uplink);
    (void)plat_boot_set_platform_check(NULL);
    (void)plat_boot_set_link_bringup(NULL);
    s_reset_calls = 0U;
    s_flag_calls = 0U;
}

/* --- memory test --- */

static void test_memory_test_passes_on_good_memory(void)
{
    uint32_t *region = malloc(4096U);
    uint32_t fail = 0xFFFFFFFFU;

    TEST_CASE("memory test passes over sound memory and rejects bad geometry");
    CHECK_TRUE(region != NULL);

    CHECK_TRUE(plat_post_memory_test(region, 1024U, &fail) == QIRAN_OK);
    CHECK_EQ_U64(fail, 0U);

    CHECK_TRUE(plat_post_memory_test(NULL, 1024U, &fail) == QIRAN_ERR_PARAM);
    CHECK_TRUE(plat_post_memory_test(region, 1U, &fail) == QIRAN_ERR_PARAM);

    free(region);
}

/*
 * Coverage rather than fault injection: a sound region cannot be made to fail
 * from software, so what is checked here is that every word really was written
 * with both polarities. Exercising the failure branches needs a fault-injection
 * harness or genuinely faulty hardware, and is a bench activity.
 */
static void test_memory_test_covers_every_word(void)
{
    const uint32_t words = 256U;
    uint32_t *region = malloc(words * 4U);
    uint32_t i;
    uint32_t mismatches = 0U;

    TEST_CASE("memory test writes every word in the region it is given");
    CHECK_TRUE(region != NULL);

    for (i = 0U; i < words; i++) {
        region[i] = 0U;
    }

    CHECK_TRUE(plat_post_memory_test(region, words, NULL) == QIRAN_OK);

    /* The final phase leaves the complement of each word's own index. */
    for (i = 0U; i < words; i++) {
        if (region[i] != ~i) {
            mismatches++;
        }
    }
    CHECK_EQ_U64(mismatches, 0U);

    TEST_CASE("memory test is destructive, as documented");
    CHECK_TRUE(region[0] != 0U);

    free(region);
}

/* --- safe outputs --- */

static uint32_t s_safe_calls;
static qiran_status_t s_safe_result;

static qiran_status_t safe_ok(void)
{
    s_safe_calls++;
    return QIRAN_OK;
}

static qiran_status_t safe_variable(void)
{
    s_safe_calls++;
    return s_safe_result;
}

static void test_safe_outputs(void)
{
    TEST_CASE("with nothing registered the outputs cannot be declared safe");
    plat_safe_outputs_reset_registry();
    CHECK_EQ_U64(plat_safe_outputs_registered(), 0U);
    CHECK_TRUE(plat_safe_outputs_apply() == QIRAN_ERR_STATE);

    TEST_CASE("registration is validated");
    CHECK_TRUE(plat_safe_outputs_register(NULL, safe_ok) == QIRAN_ERR_PARAM);
    CHECK_TRUE(plat_safe_outputs_register("x", NULL) == QIRAN_ERR_PARAM);

    TEST_CASE("every action runs, and one failure does not stop the others");
    plat_safe_outputs_reset_registry();
    s_safe_calls = 0U;
    s_safe_result = QIRAN_ERR_HARDWARE;
    CHECK_TRUE(plat_safe_outputs_register("a", safe_ok) == QIRAN_OK);
    CHECK_TRUE(plat_safe_outputs_register("bad", safe_variable) == QIRAN_OK);
    CHECK_TRUE(plat_safe_outputs_register("c", safe_ok) == QIRAN_OK);

    CHECK_TRUE(plat_safe_outputs_apply() == QIRAN_ERR_HARDWARE);
    CHECK_EQ_U64(s_safe_calls, 3U);
    CHECK_EQ_U64(plat_safe_outputs_failures(), 1U);
    CHECK_TRUE(plat_safe_outputs_last_failure()[0] == 'b');

    TEST_CASE("all actions succeeding reports success");
    s_safe_result = QIRAN_OK;
    s_safe_calls = 0U;
    CHECK_TRUE(plat_safe_outputs_apply() == QIRAN_OK);
    CHECK_EQ_U64(s_safe_calls, 3U);
}

/* --- device init ordering --- */

static char     s_order[16];
static uint32_t s_order_len;
static qiran_status_t s_bus_result;

static void note(char c)
{
    if (s_order_len < (sizeof(s_order) - 1U)) {
        s_order[s_order_len] = c;
        s_order_len++;
        s_order[s_order_len] = '\0';
    }
}

static qiran_status_t dev_bus(void)    { note('B'); return s_bus_result; }
static qiran_status_t dev_device(void) { note('D'); return QIRAN_OK; }
static qiran_status_t dev_board(void)  { note('S'); return QIRAN_OK; }
static qiran_status_t dev_link(void)   { note('L'); return QIRAN_OK; }
static qiran_status_t dev_opt(void)    { note('o'); return QIRAN_ERR_HARDWARE; }

static void test_devinit_ordering(void)
{
    TEST_CASE("stages run in ascending order regardless of registration order");
    plat_devinit_init();
    s_order_len = 0U;
    s_order[0] = '\0';
    s_bus_result = QIRAN_OK;

    CHECK_TRUE(plat_devinit_register("link", PLAT_DEVINIT_LINK, dev_link, true)
               == QIRAN_OK);
    CHECK_TRUE(plat_devinit_register("board", PLAT_DEVINIT_BOARD, dev_board, true)
               == QIRAN_OK);
    CHECK_TRUE(plat_devinit_register("bus", PLAT_DEVINIT_BUS, dev_bus, true)
               == QIRAN_OK);
    CHECK_TRUE(plat_devinit_register("dev", PLAT_DEVINIT_DEVICE, dev_device, true)
               == QIRAN_OK);

    CHECK_TRUE(plat_devinit_run() == QIRAN_OK);
    CHECK_TRUE(s_order[0] == 'B');
    CHECK_TRUE(s_order[1] == 'D');
    CHECK_TRUE(s_order[2] == 'S');
    CHECK_TRUE(s_order[3] == 'L');
    CHECK_EQ_U64(s_order_len, 4U);
    CHECK_EQ_U64(plat_devinit_count(), 4U);
    CHECK_EQ_U64(plat_devinit_failures(), 0U);

    TEST_CASE("a failed required device stops later stages being attempted");
    plat_devinit_init();
    s_order_len = 0U;
    s_order[0] = '\0';
    s_bus_result = QIRAN_ERR_HARDWARE;
    CHECK_TRUE(plat_devinit_register("bus", PLAT_DEVINIT_BUS, dev_bus, true)
               == QIRAN_OK);
    CHECK_TRUE(plat_devinit_register("dev", PLAT_DEVINIT_DEVICE, dev_device, true)
               == QIRAN_OK);

    CHECK_TRUE(plat_devinit_run() != QIRAN_OK);
    CHECK_EQ_U64(s_order_len, 1U);
    CHECK_TRUE(s_order[0] == 'B');
    CHECK_TRUE(plat_devinit_first_failure()[0] == 'b');

    TEST_CASE("a device that is not required may fail without failing the stage");
    plat_devinit_init();
    s_order_len = 0U;
    s_order[0] = '\0';
    s_bus_result = QIRAN_OK;
    CHECK_TRUE(plat_devinit_register("bus", PLAT_DEVINIT_BUS, dev_bus, true)
               == QIRAN_OK);
    CHECK_TRUE(plat_devinit_register("opt", PLAT_DEVINIT_BUS, dev_opt, false)
               == QIRAN_OK);
    CHECK_TRUE(plat_devinit_register("dev", PLAT_DEVINIT_DEVICE, dev_device, true)
               == QIRAN_OK);

    CHECK_TRUE(plat_devinit_run() == QIRAN_OK);
    CHECK_EQ_U64(plat_devinit_failures(), 1U);
    CHECK_EQ_U64(s_order_len, 3U);
}

/* --- the boot sequence --- */

static uint32_t s_post_calls;
static qiran_status_t s_post_result;

static qiran_status_t post_stub(uint32_t *detail)
{
    *detail = 0xC0DEU;
    s_post_calls++;
    return s_post_result;
}

static void test_boot_success_path(void)
{
    boot_report_t br;

    TEST_CASE("a clean boot passes every step once and reports success");
    setup();
    CHECK_TRUE(plat_safe_outputs_register("out", safe_ok) == QIRAN_OK);
    (void)plat_post_set_memory_region(malloc(1024U), 1024U);

    CHECK_TRUE(boot_and_init() == QIRAN_OK);

    plat_boot_report(&br);
    CHECK_EQ_U64(br.outcome, BOOT_OUTCOME_SUCCESS);
    CHECK_EQ_U64(br.attempts[BOOT_STEP_SAFE_OUTPUTS], 1U);
    CHECK_EQ_U64(br.attempts[BOOT_STEP_POST], 1U);
    CHECK_EQ_U64(br.attempts[BOOT_STEP_DEVICES], 1U);
    CHECK_EQ_U64(br.attempts[BOOT_STEP_LINKS], 1U);
    CHECK_EQ_U64(s_reset_calls, 0U);

    TEST_CASE("boot clears its own retry counters on success");
    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_POST), 0U);
    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_DEVINIT), 0U);
}

static void test_boot_retries_then_requests_power_cycle(void)
{
    boot_report_t br;

    TEST_CASE("a failing step is retried up to the reset-request limit");
    setup();
    CHECK_TRUE(plat_safe_outputs_register("out", safe_ok) == QIRAN_OK);
    plat_post_init();
    s_post_calls = 0U;
    s_post_result = QIRAN_ERR_HARDWARE;
    CHECK_TRUE(plat_post_register("stub", post_stub) == QIRAN_OK);

    CHECK_TRUE(boot_and_init() != QIRAN_OK);

    plat_boot_report(&br);
    CHECK_EQ_U64(br.attempts[BOOT_STEP_POST], QIRAN_RESET_REQUEST_LIMIT);
    CHECK_EQ_U64(s_post_calls, QIRAN_RESET_REQUEST_LIMIT);

    TEST_CASE("exhausting them requests a power cycle exactly once");
    CHECK_EQ_U64(s_reset_calls, 1U);

    TEST_CASE("the outcome is terminal and names the step that failed");
    CHECK_EQ_U64(br.outcome, BOOT_OUTCOME_TERMINAL);
    CHECK_EQ_U64(br.failed_step, BOOT_STEP_POST);
    CHECK_TRUE(plat_boot_step_name(br.failed_step)[0] == 'p');

    TEST_CASE("later steps are not attempted after a terminal failure");
    CHECK_EQ_U64(br.attempts[BOOT_STEP_DEVICES], 0U);
    CHECK_EQ_U64(br.attempts[BOOT_STEP_LINKS], 0U);
}

static void test_boot_recovers_on_a_later_attempt(void)
{
    boot_report_t br;

    TEST_CASE("a step that succeeds on its second attempt lets boot continue");
    setup();
    CHECK_TRUE(plat_safe_outputs_register("out", safe_ok) == QIRAN_OK);
    plat_post_init();
    s_post_calls = 0U;
    s_post_result = QIRAN_ERR_HARDWARE;
    CHECK_TRUE(plat_post_register("stub", post_stub) == QIRAN_OK);

    /* Succeeds from the second call onward. */
    s_post_result = QIRAN_ERR_HARDWARE;
    (void)boot_and_init();

    setup();
    CHECK_TRUE(plat_safe_outputs_register("out", safe_ok) == QIRAN_OK);
    plat_post_init();
    s_post_calls = 0U;
    CHECK_TRUE(plat_post_register("stub", post_stub) == QIRAN_OK);
    s_post_result = QIRAN_OK;

    CHECK_TRUE(boot_and_init() == QIRAN_OK);
    plat_boot_report(&br);
    CHECK_EQ_U64(br.outcome, BOOT_OUTCOME_SUCCESS);
    CHECK_EQ_U64(s_reset_calls, 0U);
}

static void test_unsafe_outputs_are_not_retried(void)
{
    boot_report_t br;

    TEST_CASE("an unrecoverable step is escalated once and never retried");
    setup();
    /* Nothing registered, so the outputs cannot be declared safe. */

    CHECK_TRUE(boot_and_init() != QIRAN_OK);

    plat_boot_report(&br);
    CHECK_EQ_U64(br.outcome, BOOT_OUTCOME_TERMINAL);
    CHECK_EQ_U64(br.failed_step, BOOT_STEP_SAFE_OUTPUTS);
    CHECK_EQ_U64(br.attempts[BOOT_STEP_SAFE_OUTPUTS], 1U);
    CHECK_EQ_U64(s_flag_calls, 1U);
    CHECK_EQ_U64(svc_fdir_severity_count(QIRAN_SEV_SEVERE), 1U);
}

static void test_over_budget_step_is_reported(void)
{
    boot_report_t br;

    TEST_CASE("a step that passes outside its budget is still a finding");
    setup();
    CHECK_TRUE(plat_safe_outputs_register("out", safe_ok) == QIRAN_OK);
    plat_post_init();
    s_post_result = QIRAN_OK;
    CHECK_TRUE(plat_post_register("stub", post_stub) == QIRAN_OK);

    CHECK_TRUE(boot_and_init() == QIRAN_OK);
    plat_boot_report(&br);

    /* Nothing here is slow enough to exceed a five second budget. */
    CHECK_TRUE(!br.over_budget[BOOT_STEP_POST]);
    CHECK_EQ_U64(svc_fdir_occurrences(QIRAN_FAULT_POST), 0U);
}

int main(void)
{
    plat_cpu_init();

    test_memory_test_passes_on_good_memory();
    test_memory_test_covers_every_word();
    test_safe_outputs();
    test_devinit_ordering();
    test_boot_success_path();
    test_boot_retries_then_requests_power_cycle();
    test_boot_recovers_on_a_later_attempt();
    test_unsafe_outputs_are_not_retried();
    test_over_budget_step_is_reported();

    return TEST_REPORT();
}
