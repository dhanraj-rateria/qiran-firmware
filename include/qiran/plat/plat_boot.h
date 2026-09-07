#ifndef QIRAN_PLAT_BOOT_H
#define QIRAN_PLAT_BOOT_H

#include "qiran/qiran_types.h"

/*
 * Brings up the cycle counter, interrupt controller, tick source and time
 * service, in that order, and starts the tick. Runs before boot_and_init()
 * because every budget boot has to honour is measured against the tick.
 */
qiran_status_t plat_early_init(void);

/*
 * Image validation, programmable-logic load, self test, device initialisation
 * and link establishment. Not yet implemented.
 */
qiran_status_t boot_and_init(void);

#endif
