#ifndef QIRAN_PLAT_REG_H
#define QIRAN_PLAT_REG_H

#include "qiran/qiran_types.h"

/*
 * Memory-mapped register access. Inline, so a register access costs the load
 * or store it describes and nothing else.
 *
 * Ordering: successive accesses to the same device are ordered by the volatile
 * qualifier, which is sufficient for a device mapped strongly ordered or as
 * device memory. Where an access to one device must be seen before an access
 * to another, place plat_reg_barrier() between them.
 */
static inline uint32_t plat_reg_read(uintptr_t base, uint32_t offset)
{
    return *(volatile uint32_t *)(base + offset);
}

static inline void plat_reg_write(uintptr_t base, uint32_t offset,
                                  uint32_t value)
{
    *(volatile uint32_t *)(base + offset) = value;
}

static inline void plat_reg_set(uintptr_t base, uint32_t offset, uint32_t mask)
{
    volatile uint32_t *r = (volatile uint32_t *)(base + offset);
    *r = *r | mask;
}

static inline void plat_reg_clear(uintptr_t base, uint32_t offset,
                                  uint32_t mask)
{
    volatile uint32_t *r = (volatile uint32_t *)(base + offset);
    *r = *r & ~mask;
}

static inline bool plat_reg_test(uintptr_t base, uint32_t offset,
                                 uint32_t mask)
{
    return (plat_reg_read(base, offset) & mask) == mask;
}

static inline void plat_reg_barrier(void)
{
#if defined(__arm__)
    __asm__ volatile("dsb" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

#endif
