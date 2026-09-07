#include "qiran/exec/exec_core.h"

#include "qiran/plat/plat_cpu.h"

/*
 * Tick handover is a single-producer / single-consumer counter pair. The
 * interrupt only ever increments s_isr_ticks; the loop keeps its own mirror in
 * s_consumed and advances it by difference. Nothing shared is ever cleared, so
 * no tick can be lost to a read-modify-write race and the loop needs no
 * critical section to collect one.
 */
static volatile uint32_t s_isr_ticks;

static uint32_t s_consumed;
static uint64_t s_elapsed_minor;
static uint32_t s_slot;
static uint64_t s_major_cycles;
static bool     s_major_due;

static uint32_t s_body_start;
static bool     s_body_started;
static uint32_t s_body_last;
static uint32_t s_body_worst;

static uint32_t s_overrun_events;
static uint32_t s_overrun_lost;
static uint32_t s_overrun_worst;

static bool advance(uint32_t pending)
{
    bool crossed = false;

    if (pending == 1U) {
        s_slot++;
        if (s_slot >= QIRAN_MINOR_PER_MAJOR) {
            s_slot = 0U;
            crossed = true;
        }
    } else {
        uint32_t i;
        for (i = 0U; i < pending; i++) {
            s_slot++;
            if (s_slot >= QIRAN_MINOR_PER_MAJOR) {
                s_slot = 0U;
                crossed = true;
            }
        }
    }

    s_elapsed_minor += (uint64_t)pending;
    return crossed;
}

void exec_init(void)
{
    s_isr_ticks = 0U;
    s_consumed = 0U;
    s_elapsed_minor = 0U;
    s_slot = 0U;
    s_major_cycles = 0U;
    s_major_due = false;

    s_body_start = 0U;
    s_body_started = false;
    s_body_last = 0U;
    s_body_worst = 0U;

    s_overrun_events = 0U;
    s_overrun_lost = 0U;
    s_overrun_worst = 0U;
}

void exec_on_minor_tick(void)
{
    s_isr_ticks++;
}

void exec_resync(void)
{
    uint32_t raw = s_isr_ticks;

    (void)advance(raw - s_consumed);
    s_consumed = raw;
    s_major_due = false;

    s_body_start = plat_cpu_cycle_count();
    s_body_started = true;
}

void exec_wait_for_minor_tick(void)
{
    uint32_t raw;
    uint32_t pending;

    if (s_body_started) {
        uint32_t body = plat_cpu_cycle_count() - s_body_start;
        s_body_last = body;
        if (body > s_body_worst) {
            s_body_worst = body;
        }
    }

    s_major_due = false;

    for (;;) {
        raw = s_isr_ticks;
        pending = raw - s_consumed;
        if (pending != 0U) {
            break;
        }
        plat_cpu_wait_for_interrupt();
    }

    /*
     * More than one tick pending means the previous loop body outran its minor
     * cycle. The missed cycles are dropped rather than replayed: replaying them
     * would compound the overrun, whereas dropping keeps the task set phase
     * locked to real time. The loss is recorded, never silent.
     */
    if (pending > 1U) {
        uint32_t lost = pending - 1U;
        s_overrun_events++;
        s_overrun_lost += lost;
        if (lost > s_overrun_worst) {
            s_overrun_worst = lost;
        }
    }

    s_consumed = raw;

    if (advance(pending)) {
        s_major_due = true;
        s_major_cycles++;
    }

    s_body_start = plat_cpu_cycle_count();
    s_body_started = true;
}

bool exec_major_tick_due(void)
{
    return s_major_due;
}

uint64_t exec_minor_tick_count(void)
{
    return s_elapsed_minor;
}

uint32_t exec_raw_tick_count(void)
{
    return s_isr_ticks;
}

uint32_t exec_slot(void)
{
    return s_slot;
}

void exec_stats_get(exec_stats_t *out)
{
    if (out == NULL) {
        return;
    }

    out->minor_cycles = s_elapsed_minor - (uint64_t)s_overrun_lost;
    out->major_cycles = s_major_cycles;
    out->overrun_events = s_overrun_events;
    out->overrun_cycles_lost = s_overrun_lost;
    out->overrun_worst = s_overrun_worst;
    out->body_cycles_last = s_body_last;
    out->body_cycles_worst = s_body_worst;
}

uint32_t exec_budget_cycles(void)
{
    return plat_cpu_cycles_per_us() * 1000U * QIRAN_MINOR_CYCLE_MS;
}

uint32_t exec_cycles_to_us(uint32_t cycles)
{
    uint32_t per_us = plat_cpu_cycles_per_us();
    return (per_us == 0U) ? 0U : (cycles / per_us);
}
