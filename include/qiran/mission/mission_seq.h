#ifndef QIRAN_MISSION_SEQ_H
#define QIRAN_MISSION_SEQ_H

#include "qiran/qiran_types.h"
#include "qiran/svc/svc_fault.h"

/*
 * The setup and operations sequence, held as a position in a linear list of
 * stages. The executive's fixed calling order is the schedule; this says only
 * which stage the sequence has reached. There is no transition graph and no
 * dispatch: a stage either continues, finishes, repeats, or sends the sequence
 * back to an earlier one, and each of those is a change to one index.
 */
typedef enum {
    STAGE_BOOT = 0,
    STAGE_PRECOND,
    STAGE_LASER_BRINGUP,
    STAGE_MRR_TUNE,
    STAGE_CROW_TUNE,
    STAGE_UMZI_TUNE,
    STAGE_DLI_LOCK,
    STAGE_SPAD_ENABLE,
    STAGE_PVS_T1,
    STAGE_PVS_T2,
    STAGE_EXPERIMENT,
    STAGE_PROCESS,
    STAGE_DATA_HANDLING,
    STAGE_COUNT
} mission_stage_t;

typedef enum {
    STEP_WORKING = 0,  /* not finished; called again next cycle */
    STEP_DONE,         /* finished; the sequence moves on */
    STEP_RETRY,        /* this attempt failed; the stage repeats */
    STEP_BACK,         /* return to out->back_to and re-run from there */
    STEP_ABORT         /* cannot proceed */
} mission_step_result_t;

typedef struct {
    mission_step_result_t result;
    mission_stage_t       back_to;
    qiran_fault_id_t      fault;
    uint32_t              detail;
} mission_step_t;

/*
 * Called once per minor cycle while its stage is current. Must return promptly
 * and must not block: it shares the minor cycle with every other task, and work
 * spanning more than one cycle returns working and keeps its own progress.
 */
typedef void (*mission_step_fn_t)(void *ctx, mission_step_t *out);

void mission_seq_init(void);

qiran_status_t mission_seq_register(mission_stage_t stage,
                                    mission_step_fn_t step, void *ctx);

/* Runs the current stage and applies what it returns. */
void mission_seq_step(void);

mission_stage_t mission_stage(void);
const char     *mission_stage_name(mission_stage_t stage);
uint32_t        mission_stage_elapsed_ms(void);
uint32_t        mission_stage_attempts(void);
uint32_t        mission_stage_entries(mission_stage_t stage);

/*
 * A stage whose retries are spent, or which cannot proceed, stops being run and
 * the sequence holds where it is. The flag reporting it has already gone out by
 * then.
 */
bool mission_stalled(void);

void             mission_abort(qiran_fault_id_t cause);
bool             mission_aborted(void);
qiran_fault_id_t mission_abort_cause(void);

uint32_t mission_reentries(void);

qiran_payload_status_t mission_payload_status(void);
void mission_payload_status_set(qiran_payload_status_t status);

#endif
