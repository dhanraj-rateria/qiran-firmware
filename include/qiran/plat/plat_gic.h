#ifndef QIRAN_PLAT_GIC_H
#define QIRAN_PLAT_GIC_H

#include "qiran/qiran_types.h"

/*
 * Interrupt priority ladder for the fixed interrupt set. Lower value is higher
 * priority. The tick is highest because every deadline in the system is
 * referenced to it, and late delivery shows up as cycle jitter that no later
 * task can recover. The watchdog sits next so the backstop cannot be starved by
 * data-path traffic. The remaining sources are ordered by how quickly their
 * source overflows if unserviced.
 */
#define PLAT_GIC_PRIO_TIMER    0x08U
#define PLAT_GIC_PRIO_WATCHDOG 0x10U
#define PLAT_GIC_PRIO_DMA      0x20U
#define PLAT_GIC_PRIO_SPW      0x28U
#define PLAT_GIC_PRIO_UART     0x30U
#define PLAT_GIC_PRIO_PL_ERROR 0x38U

#define PLAT_GIC_TRIGGER_LEVEL_HIGH  0x01U
#define PLAT_GIC_TRIGGER_EDGE_RISING 0x03U

typedef void (*plat_gic_handler_t)(void *ref);

qiran_status_t plat_gic_init(void);

qiran_status_t plat_gic_connect(uint32_t irq_id,
                                plat_gic_handler_t handler,
                                void *ref,
                                uint8_t priority,
                                uint8_t trigger);

void plat_gic_enable(uint32_t irq_id);
void plat_gic_disable(uint32_t irq_id);

/* Enables interrupt delivery to the core. Call once, after all sources are connected. */
void plat_gic_start(void);

/* Shared controller instance, for Xilinx drivers that require it. */
void *plat_gic_instance(void);

#endif
