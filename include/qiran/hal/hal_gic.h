#ifndef QIRAN_HAL_GIC_H
#define QIRAN_HAL_GIC_H

#include "qiran/qiran_types.h"

/*
 * Register-level interrupt controller access used by peripheral drivers.
 * Owner: Major Task 2. Not yet implemented.
 */
qiran_status_t hal_gic_init(void);

#endif
