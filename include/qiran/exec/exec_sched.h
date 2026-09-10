#ifndef QIRAN_EXEC_SCHED_H
#define QIRAN_EXEC_SCHED_H

#include "qiran/qiran_config.h"
#include "qiran/qiran_types.h"

/*
 * The task table. Call order stays where it belongs, written out literally in
 * the main loop; this module holds only what each task is, how often it is
 * meant to run, and how long it actually takes. There is no dispatcher here:
 * nothing in this module decides what runs next, and no task is reached through
 * a pointer stored in a table.
 */
typedef enum {
    EXEC_TASK_INTERRUPTS = 0,
    EXEC_TASK_HEALTH_CHECK,
    EXEC_TASK_COMMS,
    EXEC_TASK_TELECOMMAND,
    EXEC_TASK_PL_CONTROL,
    EXEC_TASK_DATA_PATH,
    EXEC_TASK_FDIR,
    EXEC_TASK_HEALTH_CONSOLIDATE,
    EXEC_TASK_TELEMETRY,
    EXEC_TASK_MAJOR,
    EXEC_TASK_WATCHDOG,
    EXEC_TASK_COUNT
} exec_task_id_t;

typedef enum {
    EXEC_CYCLE_MINOR = 0,  /* every minor cycle */
    EXEC_CYCLE_MAJOR,      /* once per major cycle, on slot zero */
    EXEC_CYCLE_SPREAD      /* every period_slots, offset by phase_slot */
} exec_cycle_class_t;

typedef struct {
    const char        *name;
    exec_cycle_class_t cycle;
    uint16_t           period_slots;
    uint16_t           phase_slot;
    /*
     * Budget from the requirements documents, in microseconds. Zero means the
     * source documents give no figure for this task, which is the case for the
     * whole of the executive's own task list.
     */
    uint32_t           estimate_us;
} exec_task_def_t;

typedef struct {
    uint32_t runs;
    uint32_t cycles_last;
    uint32_t cycles_worst;
    uint64_t cycles_total;
} exec_task_stat_t;

typedef struct {
    /*
     * Worst case over every slot in the major cycle, taking for each slot only
     * the tasks actually due in it. Summing every task instead would assume
     * spread tasks always coincide, which is the thing spreading exists to
     * prevent, and would make the remedy for a failed check unmeasurable.
     */
    uint32_t worst_slot_cycles;
    uint32_t worst_slot;
    uint32_t budget_cycles;
    uint32_t margin_cycles;
    uint32_t estimate_sum_us;
    bool     fits;
    bool     complete;   /* every task due every cycle has run at least once */
} exec_sched_check_t;

void exec_sched_init(void);

void exec_task_enter(exec_task_id_t id);
void exec_task_leave(exec_task_id_t id);

/* True when the current slot is one this task is scheduled for. */
bool exec_task_due(exec_task_id_t id);

const exec_task_def_t *exec_task_def(exec_task_id_t id);
void                   exec_task_stat_get(exec_task_id_t id, exec_task_stat_t *out);
uint32_t               exec_task_mean_cycles(exec_task_id_t id);

void exec_sched_check(exec_sched_check_t *out);

/*
 * Wraps one task so its execution time is measured. The call it wraps is
 * written out at the call site, so the loop still reads as a fixed sequence of
 * calls in a fixed order.
 */
#if QIRAN_INSTRUMENT
#define EXEC_RUN(id, call)     \
    do {                       \
        exec_task_enter(id);   \
        call;                  \
        exec_task_leave(id);   \
    } while (0)
#else
#define EXEC_RUN(id, call) do { call; } while (0)
#endif

#endif
