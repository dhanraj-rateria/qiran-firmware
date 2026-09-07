#ifndef QIRAN_PLAT_SAFE_OUTPUTS_H
#define QIRAN_PLAT_SAFE_OUTPUTS_H

#include "qiran/qiran_types.h"

/*
 * Drives every actuator output to its safe default before any control path runs.
 * Owner: Major Task 1. Not yet implemented.
 */
qiran_status_t plat_safe_outputs_init(void);

#endif
