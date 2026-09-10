/*
 * Compiled in so per-task worst cases can be preset, which is how the slot
 * analysis is exercised against known figures rather than against whatever the
 * host happens to measure.
 */
#include "../../src/exec/exec_sched.c"

#include "test_framework.h"

static void tick_to_slot(uint32_t slot)
{
    exec_init();
    while (exec_slot() != slot) {
        exec_on_minor_tick();
        exec_wait_for_minor_tick();
    }
}

static void burn(uint32_t n)
{
    volatile uint32_t sink = 0U;
    uint32_t i;
    for (i = 0U; i < n; i++) {
        sink += i;
    }
}

static void test_task_table_shape(void)
{
    uint32_t i;

    TEST_CASE("every task is named, and the loop's seven run every minor cycle");

    CHECK_EQ_U64(EXEC_TASK_COUNT, 11U);

    for (i = 0U; i < (uint32_t)EXEC_TASK_COUNT; i++) {
        const exec_task_def_t *def = exec_task_def((exec_task_id_t)i);

        CHECK_TRUE(def != NULL);
        CHECK_TRUE(def->name != NULL);
    }

    for (i = 0U; i < (uint32_t)EXEC_TASK_COUNT; i++) {
        exec_cycle_class_t cls = exec_task_def((exec_task_id_t)i)->cycle;

        CHECK_TRUE((i == (uint32_t)EXEC_TASK_MAJOR)
                   ? (cls == EXEC_CYCLE_MAJOR) : (cls == EXEC_CYCLE_MINOR));
    }

    CHECK_TRUE(exec_task_def(EXEC_TASK_COUNT) == NULL);
}

static void test_measurement(void)
{
    exec_task_stat_t st;

    TEST_CASE("a task's execution time is measured, and the peak retained");
    exec_sched_init();

    exec_task_stat_get(EXEC_TASK_HEALTH_CHECK, &st);
    CHECK_EQ_U64(st.runs, 0U);
    CHECK_EQ_U64(st.cycles_worst, 0U);
    CHECK_EQ_U64(exec_task_mean_cycles(EXEC_TASK_HEALTH_CHECK), 0U);

    EXEC_RUN(EXEC_TASK_HEALTH_CHECK, burn(50000U));
    exec_task_stat_get(EXEC_TASK_HEALTH_CHECK, &st);
    CHECK_EQ_U64(st.runs, 1U);
    CHECK_TRUE(st.cycles_last > 0U);
    CHECK_EQ_U64(st.cycles_worst, st.cycles_last);

    TEST_CASE("a longer run raises the peak, a shorter one does not lower it");
    EXEC_RUN(EXEC_TASK_HEALTH_CHECK, burn(200000U));
    exec_task_stat_get(EXEC_TASK_HEALTH_CHECK, &st);
    CHECK_EQ_U64(st.runs, 2U);
    CHECK_TRUE(st.cycles_worst >= st.cycles_last);

    {
        uint32_t peak = st.cycles_worst;

        EXEC_RUN(EXEC_TASK_HEALTH_CHECK, burn(10U));
        exec_task_stat_get(EXEC_TASK_HEALTH_CHECK, &st);
        CHECK_EQ_U64(st.cycles_worst, peak);
        CHECK_EQ_U64(st.runs, 3U);
    }

    TEST_CASE("the mean sits between the shortest and the longest run");
    CHECK_TRUE(exec_task_mean_cycles(EXEC_TASK_HEALTH_CHECK) <= st.cycles_worst);
    CHECK_TRUE(exec_task_mean_cycles(EXEC_TASK_HEALTH_CHECK) > 0U);

    TEST_CASE("measuring one task does not disturb another");
    exec_task_stat_get(EXEC_TASK_COMMS, &st);
    CHECK_EQ_U64(st.runs, 0U);

    TEST_CASE("an out-of-range identifier is ignored rather than corrupting");
    exec_task_enter(EXEC_TASK_COUNT);
    exec_task_leave(EXEC_TASK_COUNT);
    CHECK_TRUE(!exec_task_due(EXEC_TASK_COUNT));
}

static void test_due_by_class(void)
{
    uint32_t slot;
    uint32_t major_hits = 0U;

    TEST_CASE("tasks due every minor cycle are due in every slot");
    for (slot = 0U; slot < QIRAN_MINOR_PER_MAJOR; slot++) {
        tick_to_slot(slot);
        CHECK_TRUE(exec_task_due(EXEC_TASK_INTERRUPTS));
        CHECK_TRUE(exec_task_due(EXEC_TASK_FDIR));

        if (exec_task_due(EXEC_TASK_MAJOR)) {
            major_hits++;
            CHECK_EQ_U64(slot, 0U);
        }
    }

    TEST_CASE("the major-cycle task is due in exactly one slot");
    CHECK_EQ_U64(major_hits, 1U);
}

static void test_spread_placement(void)
{
    exec_task_def_t spread = { "s", EXEC_CYCLE_SPREAD, 5U, 3U, 0U };
    exec_task_def_t every  = { "e", EXEC_CYCLE_MINOR,  1U, 0U, 0U };
    exec_task_def_t major  = { "m", EXEC_CYCLE_MAJOR,  QIRAN_MINOR_PER_MAJOR, 0U, 0U };
    exec_task_def_t broken = { "b", EXEC_CYCLE_SPREAD, 0U, 0U, 0U };
    uint32_t slot;
    uint32_t spread_hits = 0U;
    uint32_t major_hits = 0U;

    TEST_CASE("placement depends only on class, period and phase");

    for (slot = 0U; slot < QIRAN_MINOR_PER_MAJOR; slot++) {
        CHECK_TRUE(due_at_slot(&every, slot));
        CHECK_TRUE(!due_at_slot(&broken, slot));

        if (due_at_slot(&spread, slot)) {
            spread_hits++;
            CHECK_EQ_U64(slot % 5U, 3U);
        }
        if (due_at_slot(&major, slot)) {
            major_hits++;
            CHECK_EQ_U64(slot, 0U);
        }
    }

    /* Slots 3, 8, 13, 18 and 23 of twenty-five. */
    CHECK_EQ_U64(spread_hits, 5U);
    CHECK_EQ_U64(major_hits, 1U);
}

static void test_worst_slot_is_a_maximum_not_a_total(void)
{
    /* Three every-cycle tasks and two costly tasks placed on different slots. */
    static const exec_task_def_t tasks[5] = {
        { "a", EXEC_CYCLE_MINOR,  1U,                    0U,  0U },
        { "b", EXEC_CYCLE_MINOR,  1U,                    0U,  0U },
        { "c", EXEC_CYCLE_MINOR,  1U,                    0U,  0U },
        { "x", EXEC_CYCLE_SPREAD, QIRAN_MINOR_PER_MAJOR, 4U,  0U },
        { "y", EXEC_CYCLE_SPREAD, QIRAN_MINOR_PER_MAJOR, 17U, 0U }
    };
    exec_task_stat_t stats[5];
    uint32_t slot = 0xFFFFFFFFU;
    uint32_t worst;
    uint32_t i;

    for (i = 0U; i < 5U; i++) {
        stats[i].runs = 1U;
        stats[i].cycles_last = 0U;
        stats[i].cycles_total = 0U;
        stats[i].cycles_worst = 0U;
    }

    TEST_CASE("two costly tasks on separate slots do not add together");
    stats[0].cycles_worst = 1000U;
    stats[1].cycles_worst = 1000U;
    stats[2].cycles_worst = 1000U;
    stats[3].cycles_worst = 6000U;
    stats[4].cycles_worst = 7000U;

    worst = worst_slot_scan(tasks, stats, 5U, &slot);

    /* Slot 17 carries the three plus the heavier of the two, and nothing else. */
    CHECK_EQ_U64(worst, (3U * 1000U) + 7000U);
    CHECK_EQ_U64(slot, 17U);

    /* A naive total over every task would have claimed this instead. */
    CHECK_TRUE(worst < ((3U * 1000U) + 6000U + 7000U));

    TEST_CASE("placing them on the same slot does make them add together");
    {
        static const exec_task_def_t together[5] = {
            { "a", EXEC_CYCLE_MINOR,  1U,                    0U, 0U },
            { "b", EXEC_CYCLE_MINOR,  1U,                    0U, 0U },
            { "c", EXEC_CYCLE_MINOR,  1U,                    0U, 0U },
            { "x", EXEC_CYCLE_SPREAD, QIRAN_MINOR_PER_MAJOR, 9U, 0U },
            { "y", EXEC_CYCLE_SPREAD, QIRAN_MINOR_PER_MAJOR, 9U, 0U }
        };

        worst = worst_slot_scan(together, stats, 5U, &slot);
        CHECK_EQ_U64(worst, (3U * 1000U) + 6000U + 7000U);
        CHECK_EQ_U64(slot, 9U);
    }
}

static void test_check_reports_completeness(void)
{
    exec_sched_check_t chk;
    uint32_t i;

    TEST_CASE("an unrun table is reported incomplete");
    exec_sched_init();
    exec_sched_check(&chk);
    CHECK_TRUE(!chk.complete);
    CHECK_EQ_U64(chk.worst_slot_cycles, 0U);
    CHECK_TRUE(chk.fits);
    CHECK_EQ_U64(chk.estimate_sum_us, 0U);

    TEST_CASE("the worst slot is slot zero, where the major task also runs");
    exec_sched_init();
    for (i = 0U; i < (uint32_t)EXEC_TASK_COUNT; i++) {
        s_stat[i].cycles_worst = 1000U;
        s_stat[i].runs = 1U;
    }
    s_stat[EXEC_TASK_MAJOR].cycles_worst = 5000U;

    exec_sched_check(&chk);
    CHECK_TRUE(chk.complete);
    CHECK_EQ_U64(chk.worst_slot, 0U);
    /* Every minor-cycle task, plus the major task in the slot it falls on. */
    CHECK_EQ_U64(chk.worst_slot_cycles,
                 (((uint32_t)EXEC_TASK_COUNT - 1U) * 1000U) + 5000U);

    TEST_CASE("one unrun every-cycle task is enough to report incomplete");
    s_stat[EXEC_TASK_COMMS].runs = 0U;
    exec_sched_check(&chk);
    CHECK_TRUE(!chk.complete);

    TEST_CASE("a null result pointer is ignored");
    exec_sched_check(NULL);
}

static void test_budget_and_margin(void)
{
    exec_sched_check_t chk;
    uint32_t budget;
    uint32_t i;

    TEST_CASE("margin is the unused part of one minor cycle");
    exec_sched_init();
    budget = exec_budget_cycles();
    CHECK_TRUE(budget > 0U);

    for (i = 0U; i < (uint32_t)EXEC_TASK_COUNT; i++) {
        s_stat[i].runs = 1U;
        s_stat[i].cycles_worst = 0U;
    }
    s_stat[EXEC_TASK_COMMS].cycles_worst = budget / 4U;

    exec_sched_check(&chk);
    CHECK_TRUE(chk.fits);
    CHECK_EQ_U64(chk.budget_cycles, budget);
    CHECK_EQ_U64(chk.margin_cycles, budget - (budget / 4U));

    TEST_CASE("exceeding the cycle is reported as not fitting, with no margin");
    s_stat[EXEC_TASK_COMMS].cycles_worst = budget + 1U;
    exec_sched_check(&chk);
    CHECK_TRUE(!chk.fits);
    CHECK_EQ_U64(chk.margin_cycles, 0U);

    TEST_CASE("exactly filling the cycle still fits");
    s_stat[EXEC_TASK_COMMS].cycles_worst = budget;
    exec_sched_check(&chk);
    CHECK_TRUE(chk.fits);
    CHECK_EQ_U64(chk.margin_cycles, 0U);
}

int main(void)
{
    plat_cpu_init();

    test_task_table_shape();
    test_measurement();
    test_due_by_class();
    test_spread_placement();
    test_worst_slot_is_a_maximum_not_a_total();
    test_check_reports_completeness();
    test_budget_and_margin();

    return TEST_REPORT();
}
