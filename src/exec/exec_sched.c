#include "qiran/exec/exec_sched.h"

#include "qiran/exec/exec_core.h"
#include "qiran/plat/plat_cpu.h"

/*
 * Estimates are zero throughout because the source documents give per-task
 * budgets for the mission stages, not for the executive's own task list. The
 * measured columns are therefore the only real figures here, which is why they
 * are collected on target rather than assembled by hand afterwards.
 */
static const exec_task_def_t k_task[EXEC_TASK_COUNT] = {
    { "state_machine",  EXEC_CYCLE_MINOR, 1U,                     0U, 0U },
    { "health",         EXEC_CYCLE_MINOR, 1U,                     0U, 0U },
    { "control_loops",  EXEC_CYCLE_MINOR, 1U,                     0U, 0U },
    { "data_path",      EXEC_CYCLE_MINOR, 1U,                     0U, 0U },
    { "comms",          EXEC_CYCLE_MINOR, 1U,                     0U, 0U },
    { "fdir",           EXEC_CYCLE_MINOR, 1U,                     0U, 0U },
    { "watchdog",       EXEC_CYCLE_MINOR, 1U,                     0U, 0U },
    { "major",          EXEC_CYCLE_MAJOR, QIRAN_MINOR_PER_MAJOR,  0U, 0U }
};

QIRAN_STATIC_ASSERT(QIRAN_ARRAY_LEN(k_task) == (size_t)EXEC_TASK_COUNT,
                    every_task_is_defined);

static exec_task_stat_t s_stat[EXEC_TASK_COUNT];
static uint32_t         s_entered[EXEC_TASK_COUNT];

void exec_sched_init(void)
{
    uint32_t i;

    for (i = 0U; i < (uint32_t)EXEC_TASK_COUNT; i++) {
        s_stat[i].runs = 0U;
        s_stat[i].cycles_last = 0U;
        s_stat[i].cycles_worst = 0U;
        s_stat[i].cycles_total = 0U;
        s_entered[i] = 0U;
    }
}

void exec_task_enter(exec_task_id_t id)
{
    if (id < EXEC_TASK_COUNT) {
        s_entered[id] = plat_cpu_cycle_count();
    }
}

void exec_task_leave(exec_task_id_t id)
{
    uint32_t elapsed;

    if (id >= EXEC_TASK_COUNT) {
        return;
    }

    elapsed = plat_cpu_cycle_count() - s_entered[id];

    s_stat[id].runs++;
    s_stat[id].cycles_last = elapsed;
    s_stat[id].cycles_total += (uint64_t)elapsed;
    if (elapsed > s_stat[id].cycles_worst) {
        s_stat[id].cycles_worst = elapsed;
    }
}

/* Pure: depends only on the definition it is given, not on module state. */
static bool due_at_slot(const exec_task_def_t *t, uint32_t slot)
{
    switch (t->cycle) {
    case EXEC_CYCLE_MINOR:
        return true;

    case EXEC_CYCLE_MAJOR:
        return slot == 0U;

    case EXEC_CYCLE_SPREAD:
    default:
        if (t->period_slots == 0U) {
            return false;
        }
        return (slot % (uint32_t)t->period_slots) == (uint32_t)t->phase_slot;
    }
}

/*
 * Worst case over every slot, counting in each slot only the tasks due in it.
 * Pure, so the analysis can be run against any table and set of measurements.
 */
static uint32_t worst_slot_scan(const exec_task_def_t *tasks,
                                const exec_task_stat_t *stats,
                                uint32_t count,
                                uint32_t *out_slot)
{
    uint32_t slot;
    uint32_t worst = 0U;
    uint32_t worst_slot = 0U;

    for (slot = 0U; slot < QIRAN_MINOR_PER_MAJOR; slot++) {
        uint32_t sum = 0U;
        uint32_t i;

        for (i = 0U; i < count; i++) {
            if (due_at_slot(&tasks[i], slot)) {
                sum += stats[i].cycles_worst;
            }
        }

        if (sum > worst) {
            worst = sum;
            worst_slot = slot;
        }
    }

    if (out_slot != NULL) {
        *out_slot = worst_slot;
    }

    return worst;
}

bool exec_task_due(exec_task_id_t id)
{
    if (id >= EXEC_TASK_COUNT) {
        return false;
    }
    return due_at_slot(&k_task[id], exec_slot());
}

const exec_task_def_t *exec_task_def(exec_task_id_t id)
{
    return (id < EXEC_TASK_COUNT) ? &k_task[id] : NULL;
}

void exec_task_stat_get(exec_task_id_t id, exec_task_stat_t *out)
{
    if ((id < EXEC_TASK_COUNT) && (out != NULL)) {
        *out = s_stat[id];
    }
}

uint32_t exec_task_mean_cycles(exec_task_id_t id)
{
    if ((id >= EXEC_TASK_COUNT) || (s_stat[id].runs == 0U)) {
        return 0U;
    }
    return (uint32_t)(s_stat[id].cycles_total / (uint64_t)s_stat[id].runs);
}

void exec_sched_check(exec_sched_check_t *out)
{
    uint32_t i;
    uint32_t worst;
    uint32_t worst_slot = 0U;
    uint32_t estimate_sum = 0U;
    bool complete = true;

    if (out == NULL) {
        return;
    }

    worst = worst_slot_scan(k_task, s_stat, (uint32_t)EXEC_TASK_COUNT,
                            &worst_slot);

    for (i = 0U; i < (uint32_t)EXEC_TASK_COUNT; i++) {
        estimate_sum += k_task[i].estimate_us;
        if ((k_task[i].cycle == EXEC_CYCLE_MINOR) && (s_stat[i].runs == 0U)) {
            complete = false;
        }
    }

    out->worst_slot_cycles = worst;
    out->worst_slot = worst_slot;
    out->budget_cycles = exec_budget_cycles();
    out->estimate_sum_us = estimate_sum;
    out->fits = worst <= out->budget_cycles;
    out->margin_cycles = out->fits ? (out->budget_cycles - worst) : 0U;
    out->complete = complete;
}
