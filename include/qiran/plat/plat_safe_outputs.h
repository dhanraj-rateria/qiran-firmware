#ifndef QIRAN_PLAT_SAFE_OUTPUTS_H
#define QIRAN_PLAT_SAFE_OUTPUTS_H

#include "qiran/qiran_types.h"

/*
 * Drives every actuator to its safe default before any control path can run.
 *
 * Actions are registered by the owners of the drive paths rather than listed
 * here, because this module must not know how to reach a digital-to-analogue
 * converter. The consequence is that having no actions registered means the
 * outputs cannot be asserted safe, which is treated as a fault rather than as
 * success: silence is the dangerous answer here, not the harmless one.
 */
#define PLAT_SAFE_OUTPUT_MAX 16U

typedef qiran_status_t (*plat_safe_action_t)(void);

qiran_status_t plat_safe_outputs_register(const char *name, plat_safe_action_t action);

/* Runs every registered action, continuing past a failure so one bad path
   cannot prevent the rest from being safed. */
qiran_status_t plat_safe_outputs_apply(void);

uint32_t plat_safe_outputs_registered(void);
uint32_t plat_safe_outputs_failures(void);
uint32_t plat_safe_outputs_applications(void);
const char *plat_safe_outputs_last_failure(void);

void plat_safe_outputs_reset_registry(void);

#endif
