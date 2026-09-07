#ifndef QIRAN_EXEC_CORE_H
#define QIRAN_EXEC_CORE_H

#include "qiran/qiran_config.h"
#include "qiran/qiran_types.h"

typedef struct {
    uint64_t minor_cycles;            /* minor cycles executed since exec_init */
    uint64_t major_cycles;            /* major cycles executed since exec_init */
    uint32_t overrun_events;          /* loop bodies that exceeded the minor cycle */
    uint32_t overrun_cycles_lost;     /* total minor cycles skipped by overruns */
    uint32_t overrun_worst;           /* most cycles lost to a single overrun */
    uint32_t body_cycles_last;        /* CPU cycles used by the previous loop body */
    uint32_t body_cycles_worst;       /* high-water mark since exec_init */
} exec_stats_t;

/* Zeroes executive state. Does not start the tick source. */
void exec_init(void);

/*
 * Called by the minor-cycle timer interrupt. The entire body is one counter
 * increment: no processing, no blocking calls, no floating point.
 */
void exec_on_minor_tick(void);

/*
 * Discards any tick backlog without charging it as an overrun, and establishes
 * the loop-body timing baseline. Required after any phase that legitimately
 * runs outside the cyclic loop, such as boot.
 */
void exec_resync(void);

/*
 * Blocks until the next minor cycle is due, then returns. Accounts for the
 * previous loop body's execution time and for any missed cycles.
 */
void exec_wait_for_minor_tick(void);

/*
 * True for the whole of a minor cycle in which the major-cycle task set is due.
 * Idempotent within a cycle; cleared by the next exec_wait_for_minor_tick().
 */
bool exec_major_tick_due(void);

/* Monotonic minor-cycle count. Main-loop context only. */
uint64_t exec_minor_tick_count(void);

/*
 * Raw tick count as maintained by the interrupt, so it advances even while the
 * cyclic loop is not running. Wraps; differences are wrap-safe by unsigned
 * subtraction. Safe to read from any context, including interrupt context.
 */
uint32_t exec_raw_tick_count(void);

/*
 * Position of the current minor cycle within the major cycle, 0 to
 * QIRAN_MINOR_PER_MAJOR-1. Phase-locked to elapsed time across overruns, so it
 * is a stable key for spreading lower-rate tasks across distinct slots.
 */
uint32_t exec_slot(void);

void exec_stats_get(exec_stats_t *out);

/* CPU cycles available to one minor cycle; the schedulability ceiling. */
uint32_t exec_budget_cycles(void);

uint32_t exec_cycles_to_us(uint32_t cycles);

#endif
