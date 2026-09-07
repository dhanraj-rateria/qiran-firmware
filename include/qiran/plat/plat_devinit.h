#ifndef QIRAN_PLAT_DEVINIT_H
#define QIRAN_PLAT_DEVINIT_H

#include "qiran/qiran_types.h"

/*
 * Dependency-ordered device initialisation. Peripheral owners register an init callback here.
 * Owner: Major Task 1. Not yet implemented.
 */
qiran_status_t plat_devinit_init(void);

#endif
