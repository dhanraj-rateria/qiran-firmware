#ifndef QIRAN_PLAT_CPU_H
#define QIRAN_PLAT_CPU_H

#include "qiran/qiran_types.h"

/*
 * Enables the cycle counter used for execution-time measurement. Must be
 * called before plat_cpu_cycle_count().
 */
void plat_cpu_init(void);

/*
 * Free-running 32-bit cycle counter. Wraps in a few seconds at flight clock
 * rates, so it is valid only for measuring intervals shorter than one wrap;
 * differences are wrap-safe by unsigned subtraction.
 */
uint32_t plat_cpu_cycle_count(void);

uint32_t plat_cpu_cycles_per_us(void);

/* Sleeps the core until any interrupt is taken. */
void plat_cpu_wait_for_interrupt(void);

/*
 * Interrupt-disabling critical section. Returns the prior mask state, which
 * must be passed back to plat_cpu_irq_restore(); nesting is therefore safe.
 */
uint32_t plat_cpu_irq_disable(void);
void     plat_cpu_irq_restore(uint32_t prior);

/* Ordering barrier between normal memory and device/ISR-visible state. */
void plat_cpu_memory_barrier(void);

/*
 * Cache maintenance over a range. Required by any test that must reach the
 * memory device itself rather than being satisfied out of cache.
 */
void plat_cpu_dcache_flush_range(volatile void *addr, uint32_t len);
void plat_cpu_dcache_invalidate_range(volatile void *addr, uint32_t len);

#endif
