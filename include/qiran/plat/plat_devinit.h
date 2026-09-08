#ifndef QIRAN_PLAT_DEVINIT_H
#define QIRAN_PLAT_DEVINIT_H

#include "qiran/qiran_types.h"

#define PLAT_DEVINIT_MAX 32U

/*
 * Ordering stages rather than a dependency graph. Fifteen or so devices form
 * four honest tiers, and a stage number is reviewable at a glance where a graph
 * is not. Within a stage, registration order applies.
 */
typedef enum {
    PLAT_DEVINIT_BUS = 0,     /* register-level transports */
    PLAT_DEVINIT_DEVICE,      /* devices reached over those transports */
    PLAT_DEVINIT_BOARD,       /* aggregated board services */
    PLAT_DEVINIT_LINK,        /* links to the observer */
    PLAT_DEVINIT_STAGE_COUNT
} plat_devinit_stage_t;

typedef qiran_status_t (*plat_devinit_fn_t)(void);

typedef struct {
    const char          *name;
    plat_devinit_stage_t stage;
    qiran_status_t       result;
    bool                 required;
    bool                 ran;
} plat_devinit_result_t;

void plat_devinit_init(void);

/*
 * A device that is not required may fail without failing initialisation; the
 * failure is still recorded and reported.
 */
qiran_status_t plat_devinit_register(const char *name,
                                     plat_devinit_stage_t stage,
                                     plat_devinit_fn_t fn,
                                     bool required);

/* Runs one stage, or every stage in ascending order. */
qiran_status_t plat_devinit_run_stage(plat_devinit_stage_t stage);
qiran_status_t plat_devinit_run(void);

uint32_t plat_devinit_count(void);
uint32_t plat_devinit_failures(void);
bool     plat_devinit_result_get(uint32_t index, plat_devinit_result_t *out);
const char *plat_devinit_first_failure(void);

#endif
