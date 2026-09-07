#ifndef QIRAN_PLAT_TIMER_H
#define QIRAN_PLAT_TIMER_H

#include "qiran/qiran_types.h"

/* Configures the minor-cycle tick source. Does not start it. */
qiran_status_t plat_timer_init(void);

void plat_timer_start(void);
void plat_timer_stop(void);

/* Reload value programmed into the tick source, for bring-up verification. */
uint32_t plat_timer_load_value(void);
uint32_t plat_timer_clock_hz(void);

/* Measured duration of the tick handler, in CPU cycles. */
void plat_timer_isr_cycles(uint32_t *last, uint32_t *worst);

#endif
