#include "qiran/plat/plat_cpu.h"

#if defined(__arm__)

#include "xil_cache.h"
#include "xparameters.h"

#define CPU_CLK_HZ ((uint32_t)XPAR_CPU_CORTEXA9_0_CPU_CLK_FREQ_HZ)

/* Performance Monitor Control: enable, reset cycle counter, no /64 divider. */
#define PMCR_ENABLE_ALL   (1U << 0)
#define PMCR_CYCLE_RESET  (1U << 2)
#define PMCR_CYCLE_DIV64  (1U << 3)
#define PMCNTEN_CYCLE     (1U << 31)

void plat_cpu_init(void)
{
    uint32_t pmcr;

    __asm__ volatile("mrc p15, 0, %0, c9, c12, 0" : "=r"(pmcr));
    pmcr |= (PMCR_ENABLE_ALL | PMCR_CYCLE_RESET);
    pmcr &= ~PMCR_CYCLE_DIV64;
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 0" : : "r"(pmcr));

    pmcr = PMCNTEN_CYCLE;
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 1" : : "r"(pmcr));
}

uint32_t plat_cpu_cycle_count(void)
{
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(v));
    return v;
}

uint32_t plat_cpu_cycles_per_us(void)
{
    return CPU_CLK_HZ / 1000000U;
}

void plat_cpu_wait_for_interrupt(void)
{
    /*
     * The barrier must precede WFI so that a flag store made by an interrupt
     * taken just before this point is visible, otherwise the core can sleep on
     * an event it has already been given.
     */
    __asm__ volatile("dsb" ::: "memory");
    __asm__ volatile("wfi" ::: "memory");
}

uint32_t plat_cpu_irq_disable(void)
{
    uint32_t cpsr;
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    __asm__ volatile("cpsid i" ::: "memory");
    return cpsr & 0x80U;
}

void plat_cpu_irq_restore(uint32_t prior)
{
    if (prior == 0U) {
        __asm__ volatile("cpsie i" ::: "memory");
    }
}

void plat_cpu_memory_barrier(void)
{
    __asm__ volatile("dmb" ::: "memory");
}

void plat_cpu_dcache_flush_range(volatile void *addr, uint32_t len)
{
    Xil_DCacheFlushRange((INTPTR)addr, len);
}

void plat_cpu_dcache_invalidate_range(volatile void *addr, uint32_t len)
{
    Xil_DCacheInvalidateRange((INTPTR)addr, len);
}

#else /* host build: portable stubs for logic-level testing */

#include <time.h>

static uint32_t s_irq_depth;

void plat_cpu_init(void) { s_irq_depth = 0U; }

uint32_t plat_cpu_cycle_count(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
}

uint32_t plat_cpu_cycles_per_us(void) { return 1000U; }

void plat_cpu_wait_for_interrupt(void) { }

uint32_t plat_cpu_irq_disable(void)
{
    uint32_t prior = (s_irq_depth != 0U) ? 0x80U : 0U;
    s_irq_depth++;
    return prior;
}

void plat_cpu_irq_restore(uint32_t prior)
{
    if (s_irq_depth != 0U) {
        s_irq_depth--;
    }
    QIRAN_UNUSED(prior);
}

void plat_cpu_memory_barrier(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

void plat_cpu_dcache_flush_range(volatile void *addr, uint32_t len)
{
    QIRAN_UNUSED(addr);
    QIRAN_UNUSED(len);
}

void plat_cpu_dcache_invalidate_range(volatile void *addr, uint32_t len)
{
    QIRAN_UNUSED(addr);
    QIRAN_UNUSED(len);
}

#endif
