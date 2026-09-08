#ifndef QIRAN_MISSION_OPS_SEQ_H
#define QIRAN_MISSION_OPS_SEQ_H

#include "qiran/mission/mission_state.h"
#include "qiran/qiran_types.h"
#include "qiran/svc/svc_fault.h"

/*
 * Drives whichever stage the current state names, once per minor cycle.
 *
 * A step never changes the state itself. It reports what it found and, where
 * the model offers a choice, names where it wants to go; the sequencer asks the
 * state model, which decides whether that is allowed. A step that names an
 * unlisted destination is refused and reported rather than obeyed.
 */
typedef enum {
    STAGE_BUSY = 0,    /* still working; will be called again next cycle */
    STAGE_PASS,        /* criteria met; advance along the nominal path */
    STAGE_RETRY,       /* attempt failed; try this stage again */
    STAGE_REDIRECT,    /* go to the named state instead */
    STAGE_FAIL         /* cannot proceed from here */
} stage_result_t;

typedef struct {
    stage_result_t     result;
    mission_state_id_t target;  /* redirect only */
    qiran_fault_id_t   fault;   /* none uses the stage's own class */
    uint32_t           detail;
} stage_outcome_t;

/*
 * Called once per minor cycle while its state is current. Must return promptly
 * and must not block: it shares a twenty millisecond budget with every other
 * task, so a step that waits on a device spends that budget for all of them.
 * Work spanning more than one cycle returns busy and keeps its own progress.
 */
typedef void (*stage_step_t)(void *ctx, stage_outcome_t *out);

void mission_ops_init(void);

qiran_status_t mission_ops_register(mission_state_id_t state,
                                    stage_step_t step,
                                    void *ctx);

/* The loop's state-machine slot. */
void state_machine_update(void);

/*
 * A stage that has spent its retries, or cannot proceed, stops being driven.
 * The state is held rather than changed, because for several stages the model
 * provides no failure exit; the flag has already been reported by then.
 */
bool               mission_ops_stalled(void);
mission_state_id_t mission_ops_stalled_state(void);
uint32_t           mission_ops_steps(void);
bool               mission_ops_driven(mission_state_id_t state);

/*
 * Stage 0. The subsystem health check and the inter-subsystem communication
 * check are supplied by the board services that can reach that hardware; with
 * neither supplied the precondition cannot be declared met, which is reported
 * rather than assumed.
 */
typedef struct {
    qiran_status_t (*dss_health)(uint32_t *detail);
    qiran_status_t (*qss_comms)(uint32_t *detail);
} mission_precond_checks_t;

qiran_status_t mission_precond_set_checks(const mission_precond_checks_t *checks);

#endif
