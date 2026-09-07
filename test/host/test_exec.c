/*
 * The executive is compiled into this translation unit so the tick-handover
 * counters can be preset, which is the only way to exercise counter wrap
 * without driving 2^32 real ticks.
 */
#include "../../src/exec/exec_core.c"

#include "qiran/svc/svc_time.h"
#include "test_framework.h"

static void tick(uint32_t n)
{
    uint32_t i;
    for (i = 0U; i < n; i++) {
        exec_on_minor_tick();
    }
}

static void burn(void)
{
    volatile uint32_t sink = 0U;
    uint32_t i;
    for (i = 0U; i < 200000U; i++) {
        sink += i;
    }
}

static void test_initial_state(void)
{
    exec_stats_t st;

    TEST_CASE("initial state is zeroed and no major cycle is due");
    exec_init();
    exec_stats_get(&st);

    CHECK_EQ_U64(exec_minor_tick_count(), 0U);
    CHECK_EQ_U64(exec_slot(), 0U);
    CHECK_TRUE(!exec_major_tick_due());
    CHECK_EQ_U64(st.minor_cycles, 0U);
    CHECK_EQ_U64(st.major_cycles, 0U);
    CHECK_EQ_U64(st.overrun_events, 0U);
}

static void test_slot_advance_and_major_boundary(void)
{
    uint32_t i;
    uint32_t major_hits = 0U;
    exec_stats_t st;

    TEST_CASE("major cycle falls on slot 0, once per 25 minor cycles");
    exec_init();

    for (i = 0U; i < 100U; i++) {
        tick(1U);
        exec_wait_for_minor_tick();

        CHECK_TRUE(exec_slot() < QIRAN_MINOR_PER_MAJOR);
        CHECK_EQ_U64(exec_slot(), exec_minor_tick_count() % QIRAN_MINOR_PER_MAJOR);

        if (exec_major_tick_due()) {
            major_hits++;
            CHECK_EQ_U64(exec_slot(), 0U);
            /* Idempotent for the whole of the cycle in which it is due. */
            CHECK_TRUE(exec_major_tick_due());
        }
    }

    CHECK_EQ_U64(exec_minor_tick_count(), 100U);
    CHECK_EQ_U64(major_hits, 4U);

    exec_stats_get(&st);
    CHECK_EQ_U64(st.major_cycles, 4U);
    CHECK_EQ_U64(st.minor_cycles, 100U);
    CHECK_EQ_U64(st.overrun_events, 0U);
}

static void test_overrun_is_counted_and_cycles_dropped(void)
{
    exec_stats_t st;

    TEST_CASE("overrun drops missed cycles, records the loss, keeps phase");
    exec_init();

    tick(1U);
    exec_wait_for_minor_tick();
    CHECK_EQ_U64(exec_minor_tick_count(), 1U);

    /* Loop body outran its slot by two cycles. */
    tick(3U);
    exec_wait_for_minor_tick();

    exec_stats_get(&st);
    CHECK_EQ_U64(st.overrun_events, 1U);
    CHECK_EQ_U64(st.overrun_cycles_lost, 2U);
    CHECK_EQ_U64(st.overrun_worst, 2U);

    /* Elapsed time tracks the tick source; executed cycles do not. */
    CHECK_EQ_U64(exec_minor_tick_count(), 4U);
    CHECK_EQ_U64(st.minor_cycles, 2U);
    CHECK_EQ_U64(exec_slot(), 4U);
}

static void test_overrun_across_major_boundary(void)
{
    uint32_t i;

    TEST_CASE("major cycle is not lost when an overrun skips over slot 0");
    exec_init();

    for (i = 0U; i < 23U; i++) {
        tick(1U);
        exec_wait_for_minor_tick();
    }
    CHECK_EQ_U64(exec_slot(), 23U);
    CHECK_TRUE(!exec_major_tick_due());

    /* Skips straight past slot 0 to slot 2. */
    tick(4U);
    exec_wait_for_minor_tick();

    CHECK_EQ_U64(exec_slot(), 2U);
    CHECK_TRUE(exec_major_tick_due());
}

static void test_resync_does_not_charge_overrun(void)
{
    exec_stats_t st;

    TEST_CASE("resync absorbs a backlog without charging it as an overrun");
    exec_init();

    tick(600U);
    exec_resync();

    exec_stats_get(&st);
    CHECK_EQ_U64(st.overrun_events, 0U);
    CHECK_EQ_U64(st.overrun_cycles_lost, 0U);
    CHECK_EQ_U64(exec_minor_tick_count(), 600U);
    CHECK_EQ_U64(exec_slot(), 600U % QIRAN_MINOR_PER_MAJOR);

    tick(1U);
    exec_wait_for_minor_tick();
    exec_stats_get(&st);
    CHECK_EQ_U64(st.overrun_events, 0U);
    CHECK_EQ_U64(exec_minor_tick_count(), 601U);
}

static void test_tick_counter_wrap(void)
{
    exec_stats_t st;

    TEST_CASE("tick handover survives 32-bit counter wrap");
    exec_init();

    s_isr_ticks = 0xFFFFFFFEU;
    s_consumed = 0xFFFFFFFEU;

    tick(1U);
    exec_wait_for_minor_tick();
    CHECK_EQ_U64(exec_minor_tick_count(), 1U);

    /* Crosses zero: raw becomes 1 while consumed is 0xFFFFFFFF. */
    tick(2U);
    exec_wait_for_minor_tick();

    exec_stats_get(&st);
    CHECK_EQ_U64(exec_minor_tick_count(), 3U);
    CHECK_EQ_U64(st.overrun_events, 1U);
    CHECK_EQ_U64(st.overrun_cycles_lost, 1U);
}

static void test_body_time_is_measured(void)
{
    exec_stats_t st;

    TEST_CASE("loop body execution time is measured and peak retained");
    exec_init();

    tick(1U);
    exec_wait_for_minor_tick();
    burn();

    tick(1U);
    exec_wait_for_minor_tick();

    exec_stats_get(&st);
    CHECK_TRUE(st.body_cycles_last > 0U);
    CHECK_TRUE(st.body_cycles_worst >= st.body_cycles_last);
    CHECK_TRUE(exec_budget_cycles() > 0U);
}

static void test_time_service(void)
{
    svc_timeout_t t;

    TEST_CASE("time service derives milliseconds from the raw tick source");
    exec_init();
    svc_time_init();

    CHECK_EQ_U64(svc_time_now_ms(), 0U);

    /* Advances even though the cyclic loop never consumes a tick. */
    tick(50U);
    CHECK_EQ_U64(svc_time_now_ms(), 50U * QIRAN_MINOR_CYCLE_MS);
    CHECK_EQ_U64(exec_minor_tick_count(), 0U);

    TEST_CASE("timeouts arm against an absolute deadline");
    svc_timeout_arm(&t, 200U);
    CHECK_TRUE(!svc_timeout_expired(&t));
    CHECK_EQ_U64(svc_timeout_remaining_ms(&t), 200U);

    tick(5U);
    CHECK_TRUE(!svc_timeout_expired(&t));
    CHECK_EQ_U64(svc_timeout_remaining_ms(&t), 100U);
    CHECK_EQ_U64(svc_timeout_elapsed_ms(&t, 200U), 100U);

    tick(5U);
    CHECK_TRUE(svc_timeout_expired(&t));
    CHECK_EQ_U64(svc_timeout_remaining_ms(&t), 0U);

    svc_timeout_disarm(&t);
    CHECK_TRUE(!svc_timeout_expired(&t));

    TEST_CASE("mission epoch correlates uptime with the supplied timebase");
    svc_time_epoch_set(1000000U);
    CHECK_TRUE(svc_time_epoch_valid());
    CHECK_EQ_U64(svc_time_epoch_ms(), 1000000U);
    tick(10U);
    CHECK_EQ_U64(svc_time_epoch_ms(), 1000000U + (10U * QIRAN_MINOR_CYCLE_MS));
}

int main(void)
{
    plat_cpu_init();

    test_initial_state();
    test_slot_advance_and_major_boundary();
    test_overrun_is_counted_and_cycles_dropped();
    test_overrun_across_major_boundary();
    test_resync_does_not_charge_overrun();
    test_tick_counter_wrap();
    test_body_time_is_measured();
    test_time_service();

    return TEST_REPORT();
}
