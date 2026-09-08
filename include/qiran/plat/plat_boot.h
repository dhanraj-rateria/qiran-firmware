#ifndef QIRAN_PLAT_BOOT_H
#define QIRAN_PLAT_BOOT_H

#include "qiran/qiran_types.h"
#include "qiran/svc/svc_fault.h"

/*
 * Brings up the cycle counter, interrupt controller, tick source and time
 * service, in that order, and starts the tick. Runs before boot_and_init()
 * because every budget boot has to honour is measured against the tick.
 */
qiran_status_t plat_early_init(void);

typedef enum {
    BOOT_STEP_SAFE_OUTPUTS = 0,
    BOOT_STEP_PLATFORM,
    BOOT_STEP_POST,
    BOOT_STEP_DEVICES,
    BOOT_STEP_LINKS,
    BOOT_STEP_COUNT
} boot_step_t;

typedef enum {
    BOOT_OUTCOME_PENDING = 0,
    BOOT_OUTCOME_SUCCESS,      /* advance out of the boot state */
    BOOT_OUTCOME_TERMINAL      /* retries exhausted, power cycle requested */
} boot_outcome_t;

typedef struct {
    boot_outcome_t outcome;
    boot_step_t    failed_step;
    uint32_t       attempts[BOOT_STEP_COUNT];
    uint32_t       duration_ms[BOOT_STEP_COUNT];
    bool           over_budget[BOOT_STEP_COUNT];
    uint32_t       total_ms;
} boot_report_t;

/*
 * Runs the boot sequence, retrying a failed step up to the reset-request limit
 * before escalating. Returns success only when every step has passed.
 */
qiran_status_t boot_and_init(void);

void        plat_boot_report(boot_report_t *out);
const char *plat_boot_step_name(boot_step_t step);

/*
 * Optional check that the platform state left by the bootloader is what was
 * expected: configuration done, boot mode, reset reason. Registered rather than
 * assumed, since what is checkable depends on the boot image design.
 */
typedef qiran_status_t (*plat_boot_check_t)(uint32_t *detail);
qiran_status_t plat_boot_set_platform_check(plat_boot_check_t check);

/* Link establishment, supplied by the communication layer when it exists. */
typedef qiran_status_t (*plat_boot_link_t)(void);
qiran_status_t plat_boot_set_link_bringup(plat_boot_link_t link);

#endif
